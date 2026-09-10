// ---------------------------------------------------------------------------
// Legacy Harmonic Saturator - dual-engine guitar drive for the Daisy Pod
//
// Two independent drive engines (A and B) that can be run in series in either
// order, or in parallel. Built for guitar straight into the Pod: input trim,
// a gate, per-model EQ, ADAA (aliasing-free) clipping, and a cabinet
// emulation on the way out.
//
// Controls
//   Knob 1            DRIVE  (pre-gain into both engines)
//   Knob 2            LEVEL  (output)
//   Encoder turn      model A
//   Hold encoder+turn model B
//   Encoder tap       bypass
//   Button 1 tap      routing: A->B / B->A / parallel sum / parallel split
//   Button 1 hold     cabinet sim on/off
//   Button 2 tap      gate: off / low / high
//   Button 2 hold     pickup load compensation on/off
//   Tap both buttons  preset mode: encoder picks a slot, encoder tap loads,
//                     button 1 saves, button 2 exits
//   Hold both buttons tuner (muted). Any tap exits
//
// Presets live in QSPI flash and the last one used is recalled at power on.
// Knobs use pickup: after a recall they stay where the preset left them until
// you actually move them.
// ---------------------------------------------------------------------------

#include "daisysp.h"
#include "daisy_pod.h"
#include "util/PersistentStorage.h"

using namespace daisysp;
using namespace daisy;

static DaisyPod pod;

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
static constexpr float kPi         = 3.14159265358979f;
static constexpr float kLevelMax   = 3.00f;   // output gain with LEVEL at full
static constexpr float kLevelCurve = 1.60f;
static constexpr float kDriveCurve = 2.00f;   // knob position -> gain taper
// Clipping already levels the signal, so compensating output for the pre-gain
// would make more drive quieter. This is only a nudge to keep the last quarter
// of the DRIVE knob from running away, and it does nothing at low drive.
static constexpr float kCompExp    = 0.15f;
static constexpr float kSeriesIn   = 0.50f;   // trim into the second engine
static constexpr float kParGain    = 0.55f;   // parallel sum trim
static constexpr float kSplitGain  = 0.85f;   // parallel crossover trim
static constexpr float kSplitHz    = 220.0f;  // parallel crossover frequency
static constexpr float kRouteFade  = 0.0015f; // ~14 ms routing crossfade
static constexpr float kFlashDecay = 0.0012f; // LED flash decay, per sample
static constexpr float kTapMs      = 300.f;   // shorter than this is a tap
static constexpr float kHoldMs     = 1000.f;  // longer than this is a hold

// Gate thresholds, as a fraction of full scale, measured on the input
static constexpr float kGateThresh[3] = {0.0f, 0.0012f, 0.0040f};
static constexpr float kGateAttack     = 0.12f;
static constexpr float kGateRelease    = 0.0006f;

// Presets
static constexpr int      kPresetCount = 8;
static constexpr uint32_t kSettingsMagic = 0x4C485331u; // "LHS1"
static constexpr float    kPickupThresh  = 0.03f;       // knob travel to take over

// Tuner. A lowpass leaves the fundamental dominant, a Schmitt trigger with
// hysteresis ignores the ripple the remaining harmonics leave on it, and the
// crossing times are interpolated so the period is not quantised to samples.
static constexpr float    kTunerCutoff  = 900.0f;
static constexpr float    kTunerHyst    = 0.25f;        // fraction of peak
static constexpr float    kTunerTol     = 0.02f;        // cluster tolerance
static constexpr int      kTunerNeed    = 5;            // periods to agree
static constexpr float    kTunerMinPeak = 0.004f;       // below this, silence
static constexpr float    kTunerMinPer  = 34.0f;        // ~1.4 kHz
static constexpr float    kTunerMaxPer  = 700.0f;       // ~69 Hz
static constexpr float    kTunerInTune  = 5.0f;         // cents
static constexpr float    kTunerNear    = 25.0f;        // cents

// std::clamp is C++17, this builds as gnu++14
static float ClampF(float x, float lo, float hi)
{
    if(x < lo)
        return lo;
    if(x > hi)
        return hi;
    return x;
}

// ---------------------------------------------------------------------------
// Biquad (RBJ cookbook, direct form 1)
// ---------------------------------------------------------------------------
struct Biquad
{
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;

    void Reset()
    {
        b0 = 1.0f;
        b1 = b2 = a1 = a2 = 0.0f;
        x1 = x2 = y1 = y2 = 0.0f;
    }

    void Set(float b0_, float b1_, float b2_, float a1_, float a2_)
    {
        b0 = b0_;
        b1 = b1_;
        b2 = b2_;
        a1 = a1_;
        a2 = a2_;
    }

    float Process(float x)
    {
        float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2      = x1;
        x1      = x;
        y2      = y1;
        y1      = y;
        return y;
    }
};

static void BiquadLowpass(Biquad& f, float fc, float q, float sr)
{
    if(fc <= 0.0f)
    {
        f.Set(1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    fc        = ClampF(fc, 10.0f, sr * 0.45f);
    float w0  = 2.0f * kPi * fc / sr;
    float cw  = cosf(w0);
    float al  = sinf(w0) / (2.0f * q);
    float a0  = 1.0f + al;
    f.Set((1.0f - cw) * 0.5f / a0,
          (1.0f - cw) / a0,
          (1.0f - cw) * 0.5f / a0,
          -2.0f * cw / a0,
          (1.0f - al) / a0);
}

static void BiquadHighpass(Biquad& f, float fc, float q, float sr)
{
    if(fc <= 0.0f)
    {
        f.Set(1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    fc        = ClampF(fc, 10.0f, sr * 0.45f);
    float w0  = 2.0f * kPi * fc / sr;
    float cw  = cosf(w0);
    float al  = sinf(w0) / (2.0f * q);
    float a0  = 1.0f + al;
    f.Set((1.0f + cw) * 0.5f / a0,
          -(1.0f + cw) / a0,
          (1.0f + cw) * 0.5f / a0,
          -2.0f * cw / a0,
          (1.0f - al) / a0);
}

static void BiquadPeak(Biquad& f, float fc, float q, float gain_db, float sr)
{
    if(gain_db == 0.0f || fc <= 0.0f)
    {
        f.Set(1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    fc        = ClampF(fc, 10.0f, sr * 0.45f);
    float w0  = 2.0f * kPi * fc / sr;
    float cw  = cosf(w0);
    float al  = sinf(w0) / (2.0f * q);
    float A   = powf(10.0f, gain_db / 40.0f);
    float a0  = 1.0f + al / A;
    f.Set((1.0f + al * A) / a0,
          -2.0f * cw / a0,
          (1.0f - al * A) / a0,
          -2.0f * cw / a0,
          (1.0f - al / A) / a0);
}

static void BiquadHighShelf(Biquad& f, float fc, float gain_db, float sr)
{
    if(gain_db == 0.0f || fc <= 0.0f)
    {
        f.Set(1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    fc        = ClampF(fc, 10.0f, sr * 0.45f);
    float w0  = 2.0f * kPi * fc / sr;
    float cw  = cosf(w0);
    float A   = powf(10.0f, gain_db / 40.0f);
    float al  = sinf(w0) * 0.5f * sqrtf(2.0f);
    float sa  = 2.0f * sqrtf(A) * al;
    float a0  = (A + 1.0f) - (A - 1.0f) * cw + sa;
    f.Set(A * ((A + 1.0f) + (A - 1.0f) * cw + sa) / a0,
          -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw) / a0,
          A * ((A + 1.0f) + (A - 1.0f) * cw - sa) / a0,
          2.0f * ((A - 1.0f) - (A + 1.0f) * cw) / a0,
          ((A + 1.0f) - (A - 1.0f) * cw - sa) / a0);
}

// One-pole highpass, used to strip the DC that asymmetric clipping generates
struct DcCut
{
    float x1, y1, r;

    void Init(float fc, float sr)
    {
        float g = 2.0f * kPi * fc / sr;
        if(g > 0.5f)
            g = 0.5f;
        r  = 1.0f - g;
        x1 = y1 = 0.0f;
    }

    float Process(float x)
    {
        float y = x - x1 + r * y1;
        x1      = x;
        y1      = y;
        return y;
    }
};

// ---------------------------------------------------------------------------
// Clippers
//
// Every shape is paired with its antiderivative so the clipper can be run
// through ADAA (antiderivative anti-aliasing). Instead of evaluating the
// curve at the sample, ADAA evaluates the average slope of the antiderivative
// across the step from the previous sample to this one, which is what the
// continuous-time result would have been. Without it, a hard clipper at 48 kHz
// folds harmonics back down as inharmonic fizz.
// ---------------------------------------------------------------------------
enum ClipType
{
    CLIP_NONE = 0,
    CLIP_SOFT, // x / sqrt(1 + x^2), smooth, never quite reaches +/-1
    CLIP_HARD, // diode / op-amp style hard clip at +/-1
    CLIP_ASYM  // soft clip that saturates earlier on the negative half
};

static float ShapeF(ClipType t, float x, float b)
{
    switch(t)
    {
        case CLIP_SOFT: return x * (1.0f / sqrtf(1.0f + x * x));
        case CLIP_HARD: return ClampF(x, -1.0f, 1.0f);
        case CLIP_ASYM:
        {
            if(x >= 0.0f)
                return x * (1.0f / sqrtf(1.0f + x * x));
            float u = b * x;
            return (1.0f / b) * (u * (1.0f / sqrtf(1.0f + u * u)));
        }
        default: return x;
    }
}

// Antiderivative of ShapeF. Offsets between branches do not matter as long as
// the two branches meet at x = 0, which is why the asymmetric one carries a
// constant.
static float ShapeAntiF(ClipType t, float x, float b)
{
    switch(t)
    {
        case CLIP_SOFT: return sqrtf(1.0f + x * x);
        case CLIP_HARD:
        {
            float a = fabsf(x);
            return a <= 1.0f ? 0.5f * x * x : a - 0.5f;
        }
        case CLIP_ASYM:
        {
            if(x >= 0.0f)
                return sqrtf(1.0f + x * x);
            float u = b * x;
            return (1.0f / (b * b)) * sqrtf(1.0f + u * u) + (1.0f - 1.0f / (b * b));
        }
        default: return 0.5f * x * x;
    }
}

static float AdaaClip(ClipType t, float x, float* x1, float b)
{
    float xp = *x1;
    *x1      = x;
    if(t == CLIP_NONE)
        return x;

    float d = x - xp;
    if(fabsf(d) < 1e-7f)
        return ShapeF(t, 0.5f * (x + xp), b);

    return (ShapeAntiF(t, x, b) - ShapeAntiF(t, xp, b)) / d;
}

// ---------------------------------------------------------------------------
// Drive models
// ---------------------------------------------------------------------------
struct Model
{
    float    pre_hp;    // highpass before the gain, Hz (tightens the low end)
    float    pre_pk_f;  // pre-gain peaking filter, Hz
    float    pre_pk_g;  // pre-gain peaking filter, dB
    float    pre_pk_q;
    ClipType c1;        // first clipper
    ClipType c2;        // second clipper, CLIP_NONE for a single stage
    float    c2_gain;   // gain between the two clippers at full DRIVE, scaled
                        // down to unity as the knob comes back
    float    asym;      // asymmetry factor for CLIP_ASYM
    float    post_lp;   // post-clip lowpass, Hz (fizz control)
    float    post_pk_f; // post-clip peaking filter, Hz
    float    post_pk_g; // post-clip peaking filter, dB
    float    post_pk_q;
    float    max_db;    // pre-gain at full DRIVE
    float    makeup;    // trims models to a similar loudness
};

static const Model kModels[] = {
    // 0 BOOST - clean preamp, no clipping. Useful on its own and as the
    //           "second model" when you want boost-into-drive or drive-into-boost.
    {25.0f, 0.0f, 0.0f, 0.7f, CLIP_NONE, CLIP_NONE, 1.0f, 1.0f, 12000.0f,
     2500.0f, 1.5f, 0.7f, 16.0f, 1.15f},
    // 1 SOFT - transparent overdrive
    {95.0f, 850.0f, 3.0f, 0.8f, CLIP_SOFT, CLIP_SOFT, 1.4f, 1.0f, 6800.0f,
     3200.0f, -2.0f, 0.8f, 36.0f, 1.15f},
    // 2 CRUNCH - op-amp style hard clip
    {130.0f, 500.0f, -2.0f, 0.7f, CLIP_HARD, CLIP_HARD, 2.0f, 1.0f, 5200.0f,
     1800.0f, 3.0f, 0.8f, 42.0f, 0.95f},
    // 3 FUZZ - scooped, dark, cavernous
    {60.0f, 700.0f, -5.0f, 0.7f, CLIP_HARD, CLIP_HARD, 3.0f, 1.0f, 4200.0f,
     120.0f, 4.0f, 0.8f, 52.0f, 0.75f},
    // 4 HIGH GAIN - asymmetric, tight, present
    {110.0f, 900.0f, 4.0f, 0.8f, CLIP_ASYM, CLIP_ASYM, 2.2f, 1.7f, 6000.0f,
     3000.0f, 3.5f, 0.8f, 50.0f, 0.90f},
};
static constexpr int kNumModels = 5;

// ---------------------------------------------------------------------------
// One drive engine
// ---------------------------------------------------------------------------
struct Engine
{
    Biquad pre_hp, pre_pk, post_lp, post_pk;
    DcCut dc;
    float   x1a, x1b;
    int     model;

    void Init(float sr)
    {
        pre_hp.Reset();
        pre_pk.Reset();
        post_lp.Reset();
        post_pk.Reset();
        dc.Init(25.0f, sr);
        x1a = x1b = 0.0f;
        model     = 0;
    }

    void SetModel(int m, float sr)
    {
        model         = m;
        const Model& p = kModels[m];
        BiquadHighpass(pre_hp, p.pre_hp, 0.7f, sr);
        BiquadPeak(pre_pk, p.pre_pk_f, p.pre_pk_q, p.pre_pk_g, sr);
        BiquadLowpass(post_lp, p.post_lp, 0.7f, sr);
        BiquadPeak(post_pk, p.post_pk_f, p.post_pk_q, p.post_pk_g, sr);
    }

    float Process(float x, float drive, float c2g, float comp)
    {
        const Model& p = kModels[model];

        float s = pre_hp.Process(x);
        s       = pre_pk.Process(s);
        s *= drive;

        s = AdaaClip(p.c1, s, &x1a, p.asym);
        if(p.c2 != CLIP_NONE)
            s = AdaaClip(p.c2, s * c2g, &x1b, p.asym);

        s = dc.Process(s);
        s = post_lp.Process(s);
        s = post_pk.Process(s);

        return s * comp * p.makeup;
    }
};

// A path owns its own pair of engines. All four are kept alive so a routing
// change can crossfade instead of re-patching the graph mid-sample.
struct Path
{
    Engine a, b;
    Biquad xo;

    void Init(float sr)
    {
        a.Init(sr);
        b.Init(sr);
        xo.Reset();
    }
};

static Path path_ab;
static Path path_ba;
static Path path_par;
static Path path_split;

// ---------------------------------------------------------------------------
// Cabinet emulation
//
// A guitar cabinet is what makes a driven guitar sound like a record instead
// of like a fuzz pedal plugged into a mixing desk. Six biquads approximating
// a closed-back 1x12: thump, a small cone dip, presence, and a steep top roll
// off that also does most of the fizz control.
// ---------------------------------------------------------------------------
struct Cabinet
{
    Biquad hp, thump, cone, presence, lp1, lp2;

    void Init(float sr)
    {
        hp.Reset();
        thump.Reset();
        cone.Reset();
        presence.Reset();
        lp1.Reset();
        lp2.Reset();

        BiquadHighpass(hp, 80.0f, 0.7f, sr);
        BiquadPeak(thump, 110.0f, 1.0f, 3.5f, sr);
        BiquadPeak(cone, 500.0f, 0.8f, -2.5f, sr);
        BiquadPeak(presence, 2600.0f, 0.9f, 4.0f, sr);
        BiquadLowpass(lp1, 6200.0f, 0.707f, sr);
        BiquadLowpass(lp2, 8500.0f, 0.707f, sr);
    }

    float Process(float x)
    {
        float y = hp.Process(x);
        y       = thump.Process(y);
        y       = cone.Process(y);
        y       = presence.Process(y);
        y       = lp1.Process(y);
        return lp2.Process(y);
    }
};

// ---------------------------------------------------------------------------
// Tuner
//
// Runs continuously off the clean input so it is ready the moment you call it
// up. Cost is two biquads and a few compares per sample.
// ---------------------------------------------------------------------------
struct Tuner
{
    Biquad   lp1, lp2;
    float    peak, prev;
    float    last_t;
    uint32_t n;
    float    per[12];
    int      pi, pcount;

    void Init(float sr)
    {
        lp1.Reset();
        lp2.Reset();
        BiquadLowpass(lp1, kTunerCutoff, 0.7f, sr);
        BiquadLowpass(lp2, kTunerCutoff, 0.7f, sr);
        Reset();
    }

    void Reset()
    {
        peak = prev = 0.0f;
        last_t      = -1.0f;
        n           = 0;
        pi = pcount = 0;
        for(int i = 0; i < 12; i++)
            per[i] = 0.0f;
    }

    void Process(float x)
    {
        float y = lp2.Process(lp1.Process(x));

        float ax = fabsf(y);
        peak     = ax > peak ? ax : peak + (ax - peak) * 0.00008f;
        float thr = kTunerHyst * peak;

        if(armed_ && peak > kTunerMinPeak && prev <= thr && y > thr)
        {
            float frac = (y != prev) ? (thr - prev) / (y - prev) : 0.0f;
            float tc   = (float)n - 1.0f + frac;
            if(last_t >= 0.0f)
            {
                float p = tc - last_t;
                if(p >= kTunerMinPer && p <= kTunerMaxPer)
                {
                    per[pi] = p;
                    pi      = (pi + 1) % 12;
                    if(pcount < 12)
                        pcount++;
                }
            }
            last_t  = tc;
            armed_  = false;
        }
        if(y < -thr)
            armed_ = true;

        prev = y;
        n++;
    }

    // Largest cluster of periods within tolerance, averaged. Rejects the odd
    // mis-trigger instead of letting it drag a median around.
    float Estimate()
    {
        if(pcount < kTunerNeed)
            return 0.0f;

        float best = 0.0f;
        int   bestn = 0;
        for(int i = 0; i < pcount; i++)
        {
            float sum = 0.0f;
            int   cnt = 0;
            for(int j = 0; j < pcount; j++)
            {
                if(fabsf(per[j] - per[i]) <= kTunerTol * per[i])
                {
                    sum += per[j];
                    cnt++;
                }
            }
            if(cnt > bestn)
            {
                bestn = cnt;
                best  = sum / (float)cnt;
            }
        }
        return bestn >= kTunerNeed ? (float)pod.AudioSampleRate() / best : 0.0f;
    }

    bool armed_ = true;
};

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------
struct PresetData
{
    int32_t model_a;
    int32_t model_b;
    int32_t route;
    int32_t gate_idx;
    int32_t cab_on;
    int32_t comp_on;
    float   drive;
    float   level;
};

struct Settings
{
    PresetData presets[kPresetCount];
    int32_t    last_slot;
    uint32_t   magic;

    // PersistentStorage only rewrites flash when the data actually changed,
    // and it needs these to decide
    bool operator==(const Settings& o) const
    {
        if(last_slot != o.last_slot || magic != o.magic)
            return false;
        for(int i = 0; i < kPresetCount; i++)
        {
            const PresetData& a = presets[i];
            const PresetData& b = o.presets[i];
            if(a.model_a != b.model_a || a.model_b != b.model_b
               || a.route != b.route || a.gate_idx != b.gate_idx
               || a.cab_on != b.cab_on || a.comp_on != b.comp_on
               || a.drive != b.drive || a.level != b.level)
                return false;
        }
        return true;
    }

    bool operator!=(const Settings& o) const { return !(*this == o); }
};

// Soft safety limiter. Transparent below the knee, hard ceiling at 1.0 so the
// DAC never sees a rail-to-rail square wave.
static float Safety(float x)
{
    const float knee = 0.7f;
    float       a    = fabsf(x);
    if(a <= knee)
        return x;
    float s = knee + (1.0f - knee) * tanhf((a - knee) / (1.0f - knee));
    return x < 0.0f ? -s : s;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static Cabinet cab;
static DcCut in_dc;
static Biquad  load_comp;
static Tuner   tuner;
static PersistentStorage<Settings> storage(pod.seed.qspi);

static int   model_a = 1; // SOFT
static int   model_b = 0; // BOOST
static int   route   = 1; // B -> A, boost into drive
static int   gate_idx   = 1;
static bool  cab_on     = true;
static bool  comp_on    = true;
static bool  bypass     = false;

static bool  preset_mode = false;
static int   slot        = 0;
static bool  tuner_on    = false;
static bool  save_req    = false;
static float tuner_hz    = 0.0f;

// Knob pickup: after a recall the stored value holds until you move the knob
static float drive_val = 0.5f, level_val = 0.5f;
static float knob_ref_d = 0.0f, knob_ref_l = 0.0f;
static bool  knob_locked = false;

// Both-button gesture: tap both for preset mode, hold both for the tuner
static bool     both_down = false, both_seen = false, both_fired = false;
static uint32_t both_t = 0;
static uint32_t tuner_poll = 0;

static float w[4] = {0.0f, 1.0f, 0.0f, 0.0f};
static float cab_mix    = 1.0f;
static float comp_mix   = 1.0f;
static float gate_gain  = 1.0f;
static float gate_env   = 0.0f;
static float out_env    = 0.0f;
static float flash      = 0.0f;

static uint32_t press_enc = 0, press_b1 = 0, press_b2 = 0;
static bool     enc_moved = false;
static bool     hold_b1 = false, hold_b2 = false;

// Applies a preset to the live state. `pickup` decides whether the stored knob
// values take over (and lock until the knobs move) or are ignored.
static const PresetData kDefaultPreset = {
    1,    // model_a: SOFT
    0,    // model_b: BOOST
    1,    // route:   B -> A
    1,    // gate:    low
    1,    // cab on
    1,    // comp on
    0.5f, // drive
    0.5f, // level
};

static void ApplyModels(float sr);

static void ApplyPreset(const PresetData& p, bool pickup)
{
    model_a  = p.model_a % kNumModels;
    model_b  = p.model_b % kNumModels;
    route    = p.route & 3;
    gate_idx = p.gate_idx % 3;
    cab_on   = p.cab_on != 0;
    comp_on  = p.comp_on != 0;
    ApplyModels(pod.AudioSampleRate());

    if(pickup)
    {
        drive_val = ClampF(p.drive, 0.0f, 1.0f);
        level_val = ClampF(p.level, 0.0f, 1.0f);
        // -1 means "not measured yet": the first audio block grabs the actual
        // knob position, so a preset recalled at boot does not unlock itself
        knob_ref_d  = -1.0f;
        knob_ref_l  = -1.0f;
        knob_locked = true;
    }
}

static void CapturePreset(PresetData& p)
{
    p.model_a  = model_a;
    p.model_b  = model_b;
    p.route    = route;
    p.gate_idx = gate_idx;
    p.cab_on   = cab_on ? 1 : 0;
    p.comp_on  = comp_on ? 1 : 0;
    p.drive    = drive_val;
    p.level    = level_val;
}

static void ApplyModels(float sr)
{
    path_ab.a.SetModel(model_a, sr);
    path_ab.b.SetModel(model_b, sr);
    path_ba.a.SetModel(model_a, sr);
    path_ba.b.SetModel(model_b, sr);
    path_par.a.SetModel(model_a, sr);
    path_par.b.SetModel(model_b, sr);
    path_split.a.SetModel(model_a, sr);
    path_split.b.SetModel(model_b, sr);
}

static void ModelColor(int m, float& r, float& g, float& b)
{
    switch(m)
    {
        case 0: r = 0.15f; g = 0.50f; b = 1.00f; break; // ice blue
        case 1: r = 1.00f; g = 0.75f; b = 0.15f; break; // amber
        case 2: r = 1.00f; g = 0.35f; b = 0.05f; break; // orange
        case 3: r = 0.85f; g = 0.10f; b = 0.60f; break; // magenta
        default: r = 0.20f; g = 1.00f; b = 0.35f; break;// green
    }
}

static void RouteColor(int r_, float& r, float& g, float& b)
{
    switch(r_)
    {
        case 0: r = 0.60f; g = 0.15f; b = 1.00f; break; // A -> B
        case 1: r = 0.10f; g = 0.80f; b = 1.00f; break; // B -> A
        case 2: r = 0.90f; g = 0.90f; b = 0.90f; break; // parallel sum
        default: r = 0.70f; g = 1.00f; b = 0.20f; break;// parallel split
    }
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t                                size)
{
    pod.ProcessAnalogControls();
    pod.ProcessDigitalControls();

    float sr = pod.AudioSampleRate();

    // --- controls ---------------------------------------------------------
    float raw_d = ClampF(pod.knob1.Process(), 0.0f, 1.0f);
    float raw_l = ClampF(pod.knob2.Process(), 0.0f, 1.0f);

    // Knob pickup: the stored preset value holds until a knob actually moves
    if(knob_locked)
    {
        if(knob_ref_d < 0.0f)
        {
            knob_ref_d = raw_d;
            knob_ref_l = raw_l;
        }
        else if(fabsf(raw_d - knob_ref_d) > kPickupThresh
                || fabsf(raw_l - knob_ref_l) > kPickupThresh)
        {
            knob_locked = false;
        }
    }
    if(!knob_locked)
    {
        drive_val = raw_d;
        level_val = raw_l;
    }

    bool b1_down = pod.button1.Pressed();
    bool b2_down = pod.button2.Pressed();

    // Both buttons together: tap opens the preset menu, hold opens the tuner
    if(b1_down && b2_down)
    {
        both_seen = true;
        if(!both_down)
        {
            both_down = true;
            both_t    = System::GetNow();
        }
        else if(!both_fired && System::GetNow() - both_t > 1200)
        {
            both_fired = true;
            tuner_on   = true;
            preset_mode = false;
            tuner.Reset();
            flash = 1.0f;
        }
    }
    else if(both_down)
    {
        if(!both_fired && System::GetNow() - both_t < 400)
        {
            if(tuner_on)
                tuner_on = false;
            else
                preset_mode = !preset_mode;
            flash = 1.0f;
        }
        both_down  = false;
        both_fired = false;
    }
    if(!b1_down && !b2_down)
        both_seen = false;

    if(tuner_on)
    {
        // Any single control backs out of the tuner
        if(pod.encoder.FallingEdge() || pod.button1.FallingEdge()
           || pod.button2.FallingEdge())
        {
            if(!both_seen)
            {
                tuner_on = false;
                flash    = 1.0f;
            }
        }
    }
    else if(preset_mode)
    {
        // Encoder picks a slot, encoder tap loads, button 1 saves, button 2 exits
        int32_t inc = pod.encoder.Increment();
        if(inc != 0)
        {
            slot += inc;
            slot = (slot % kPresetCount + kPresetCount) % kPresetCount;
            flash = 1.0f;
        }
        if(pod.encoder.FallingEdge() && !both_seen)
        {
            Settings& s = storage.GetSettings();
            ApplyPreset(s.presets[slot], true);
            s.last_slot = slot;
            flash       = 1.0f;
        }
        if(pod.button1.FallingEdge() && !both_seen)
        {
            Settings& s = storage.GetSettings();
            CapturePreset(s.presets[slot]);
            s.last_slot = slot;
            save_req    = true;
            flash       = 1.0f;
        }
        if(pod.button2.FallingEdge() && !both_seen)
        {
            preset_mode = false;
            flash       = 1.0f;
        }
    }
    else
    {
        // Encoder: turn picks model A, held-turn picks model B, tap bypasses
        int32_t inc = pod.encoder.Increment();
        if(inc != 0)
        {
            if(pod.encoder.Pressed())
            {
                model_b += inc;
                model_b  = (model_b % kNumModels + kNumModels) % kNumModels;
                enc_moved = true;
                ApplyModels(sr);
            }
            else
            {
                model_a += inc;
                model_a  = (model_a % kNumModels + kNumModels) % kNumModels;
                ApplyModels(sr);
            }
        }
        if(pod.encoder.RisingEdge())
        {
            press_enc = System::GetNow();
            enc_moved = false;
        }
        if(pod.encoder.FallingEdge() && !both_seen)
        {
            if(System::GetNow() - press_enc < (uint32_t)kTapMs && !enc_moved)
                bypass = !bypass;
        }

        // Button 1: tap cycles the routing, hold toggles the cabinet sim
        if(pod.button1.RisingEdge())
            press_b1 = System::GetNow();
        if(pod.button1.FallingEdge() && !both_seen)
        {
            if(System::GetNow() - press_b1 < (uint32_t)kHoldMs)
            {
                route = (route + 1) & 3;
                flash = 1.0f;
            }
            hold_b1 = false;
        }
        if(pod.button1.Pressed()
           && System::GetNow() - press_b1 > (uint32_t)kHoldMs)
        {
            if(!hold_b1)
            {
                hold_b1  = true;
                cab_on   = !cab_on;
                flash    = 1.0f;
            }
        }

        // Button 2: tap cycles the gate, hold toggles load compensation
        if(pod.button2.RisingEdge())
            press_b2 = System::GetNow();
        if(pod.button2.FallingEdge() && !both_seen)
        {
            if(System::GetNow() - press_b2 < (uint32_t)kHoldMs)
            {
                gate_idx = (gate_idx + 1) % 3;
                flash    = 1.0f;
            }
            hold_b2 = false;
        }
        if(pod.button2.Pressed()
           && System::GetNow() - press_b2 > (uint32_t)kHoldMs)
        {
            if(!hold_b2)
            {
                hold_b2  = true;
                comp_on  = !comp_on;
                flash    = 1.0f;
            }
        }
    }

    // --- derived values, once per block ----------------------------------
    float t  = powf(drive_val, kDriveCurve);
    float ga = powf(10.0f, t * kModels[model_a].max_db / 20.0f);
    float gb = powf(10.0f, t * kModels[model_b].max_db / 20.0f);
    float ca = powf(ga, -kCompExp);
    float cb = powf(gb, -kCompExp);

    // Inter-stage gain only opens up as you turn DRIVE up, otherwise a cascaded
    // model would be a huge clean boost with the knob at zero
    float c2ga = 1.0f + (kModels[model_a].c2_gain - 1.0f) * t;
    float c2gb = 1.0f + (kModels[model_b].c2_gain - 1.0f) * t;

    // The second engine in a series pair already sees a near-unity signal, so
    // it runs at half the dB, half the inter-stage gain, and gets trimmed on
    // the way in
    float ga2      = powf(ga, 0.5f);
    float gb2      = powf(gb, 0.5f);
    float ca2      = powf(ga2, -kCompExp);
    float cb2      = powf(gb2, -kCompExp);
    float c2ga2    = 1.0f + (kModels[model_a].c2_gain - 1.0f) * t * 0.5f;
    float c2gb2    = 1.0f + (kModels[model_b].c2_gain - 1.0f) * t * 0.5f;
    float level_g  = kLevelMax * powf(level_val, kLevelCurve);
    float gate_thr = kGateThresh[gate_idx];

    for(int i = 0; i < 4; i++)
        fonepole(w[i], i == route ? 1.0f : 0.0f, kRouteFade);
    fonepole(cab_mix, cab_on ? 1.0f : 0.0f, 0.002f);
    fonepole(comp_mix, comp_on ? 1.0f : 0.0f, 0.002f);

    // --- per sample -------------------------------------------------------
    for(size_t i = 0; i < size; i += 2)
    {
        // The tuner always listens to the clean input, so it is ready the
        // instant you call it up
        tuner.Process(in[i]);
        if((++tuner_poll & 255) == 0)
            tuner_hz = tuner.Estimate();

        if(tuner_on)
        {
            out[i] = out[i + 1] = 0.0f;
            flash -= flash * kFlashDecay;
            continue;
        }

        if(bypass)
        {
            out[i]     = in[i];
            out[i + 1] = in[i + 1];
            continue;
        }

        float x = in[i];

        x = in_dc.Process(x);

        // Undo some of the dulling you get from plugging pickups into a line
        // input instead of a 1 M ohm amp input
        if(comp_mix > 0.0005f)
        {
            float xc = load_comp.Process(x);
            x        = x + (xc - x) * comp_mix;
        }

        // Gate, measured before the gain so it is not amplifying its own noise
        float ax = fabsf(x);
        gate_env = ax > gate_env ? ax : gate_env + (ax - gate_env) * 0.001f;
        if(gate_thr > 0.0f)
        {
            float target = gate_env > gate_thr ? 1.0f : 0.0f;
            fonepole(gate_gain,
                     target,
                     target > gate_gain ? kGateAttack : kGateRelease);
        }
        else
        {
            gate_gain = 1.0f;
        }
        x *= gate_gain;

        float y = 0.0f;

        // A -> B
        if(w[0] > 0.0005f)
        {
            float s = path_ab.a.Process(x, ga, c2ga, ca);
            y += w[0] * path_ab.b.Process(s * kSeriesIn, gb2, c2gb2, cb2);
        }
        // B -> A
        if(w[1] > 0.0005f)
        {
            float s = path_ba.b.Process(x, gb, c2gb, cb);
            y += w[1] * path_ba.a.Process(s * kSeriesIn, ga2, c2ga2, ca2);
        }
        // parallel, both engines across the full range
        if(w[2] > 0.0005f)
        {
            y += w[2] * kParGain
                 * (path_par.a.Process(x, ga, c2ga, ca)
                    + path_par.b.Process(x, gb, c2gb, cb));
        }
        // parallel, split at the crossover so the low end stays tight
        if(w[3] > 0.0005f)
        {
            float lp = path_split.xo.Process(x);
            float hp = x - lp;
            y += w[3] * kSplitGain
                 * (path_split.a.Process(lp, ga, c2ga, ca)
                    + path_split.b.Process(hp, gb, c2gb, cb));
        }

        float c = cab.Process(y);
        y       = y + (c - y) * cab_mix;
        y *= level_g;
        y = Safety(y);

        out[i]     = y;
        out[i + 1] = y;

        float ay = fabsf(y);
        out_env  = ay > out_env ? ay : out_env + (ay - out_env) * 0.0008f;
        flash -= flash * kFlashDecay;
    }

    // --- LEDs -------------------------------------------------------------
    if(tuner_on)
    {
        // LED 1 is the flat side, LED 2 the sharp side, both green in tune
        float r = 0.0f, g = 0.0f, b = 0.0f;
        if(tuner_hz > 0.0f)
        {
            float n     = 69.0f + 12.0f * log2f(tuner_hz / 440.0f);
            float cents = (n - roundf(n)) * 100.0f;
            float ac    = fabsf(cents);
            float bright = 0.45f + 0.55f * (ac > 50.0f ? 1.0f : ac / 50.0f);

            if(ac <= kTunerInTune)
            {
                r = 0.05f; g = 1.0f; b = 0.15f;
                pod.led1.Set(r, g, b);
                pod.led2.Set(r, g, b);
            }
            else
            {
                // Amber when close, red when far out
                float near = ac <= kTunerNear ? 1.0f : 0.0f;
                r = bright;
                g = bright * (near ? 0.55f : 0.10f);
                b = bright * 0.03f;
                if(cents < 0.0f)
                {
                    pod.led1.Set(r, g, b);
                    pod.led2.Set(0.0f, 0.0f, 0.0f);
                }
                else
                {
                    pod.led1.Set(0.0f, 0.0f, 0.0f);
                    pod.led2.Set(r, g, b);
                }
            }
        }
        else
        {
            // No note: slow dim pulse on both
            float p = 0.15f + 0.15f * flash;
            pod.led1.Set(p, p, p);
            pod.led2.Set(p, p, p);
        }
        flash -= flash * kFlashDecay;
    }
    else if(preset_mode)
    {
        // One hue per slot, pulsing to show you are in the menu
        float h   = (float)slot / (float)kPresetCount;
        float r   = 0.5f + 0.5f * cosf(2.0f * kPi * h);
        float g   = 0.5f + 0.5f * cosf(2.0f * kPi * (h - 0.333f));
        float bb  = 0.5f + 0.5f * cosf(2.0f * kPi * (h + 0.333f));
        float lvl = 0.35f + 0.65f * flash;
        pod.led1.Set(r * lvl, g * lvl, bb * lvl);
        pod.led2.Set(flash * 0.8f, flash * 0.8f, flash * 0.8f);
        flash -= flash * kFlashDecay;
    }
    else if(bypass)
    {
        pod.led1.Set(0.06f, 0.0f, 0.0f);
        pod.led2.Set(0.0f, 0.0f, 0.0f);
    }
    else
    {
        float r, g, b;
        bool  editing_b = pod.encoder.Pressed();
        ModelColor(editing_b ? model_b : model_a, r, g, b);

        float bright = 0.30f + 0.70f * (out_env > 1.0f ? 1.0f : out_env);
        if(out_env > 0.98f)
            r = r > 1.0f ? 1.0f : r + (1.0f - r) * 0.6f;
        pod.led1.Set(r * bright, g * bright, b * bright);

        RouteColor(route, r, g, b);
        float b2 = 0.45f + 0.55f * flash;
        pod.led2.Set(r * b2 + flash * 0.5f,
                     g * b2 + flash * 0.5f,
                     b * b2 + flash * 0.5f);
    }

    pod.UpdateLeds();
}

int main(void)
{
    pod.Init();
    pod.SetAudioBlockSize(4);

    float sr = pod.AudioSampleRate();

    path_ab.Init(sr);
    path_ba.Init(sr);
    path_par.Init(sr);
    path_split.Init(sr);

    BiquadLowpass(path_split.xo, kSplitHz, 0.707f, sr);

    cab.Init(sr);
    in_dc.Init(20.0f, sr);

    // A high shelf on the way in. Compensates for the treble a guitar loses
    // when it is loaded by a line input instead of an amp. Turn it off with
    // button 2 if you have a buffer pedal in front.
    load_comp.Reset();
    BiquadHighShelf(load_comp, 2200.0f, 4.5f, sr);

    tuner.Init(sr);

    ApplyModels(sr);

    // Presets. On a fresh chip (magic mismatch) the defaults are committed so
    // the first boot has something sane to recall.
    Settings defaults;
    for(int i = 0; i < kPresetCount; i++)
        defaults.presets[i] = kDefaultPreset;
    defaults.last_slot = 0;
    defaults.magic     = kSettingsMagic;

    storage.Init(defaults);

    Settings& st = storage.GetSettings();
    if(st.magic != kSettingsMagic || st.last_slot < 0
       || st.last_slot >= kPresetCount)
    {
        for(int i = 0; i < kPresetCount; i++)
            st.presets[i] = kDefaultPreset;
        st.last_slot = 0;
        st.magic     = kSettingsMagic;
        storage.Save();
    }
    else
    {
        slot = st.last_slot;
        ApplyPreset(st.presets[slot], true);
    }

    pod.StartAdc();
    pod.StartAudio(AudioCallback);

    while(1)
    {
        // Flash writes stall, so they happen here rather than in the callback
        if(save_req)
        {
            save_req = false;
            storage.Save();
        }
    }
}
