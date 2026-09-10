#include "daisysp.h"
#include "daisy_pod.h"

using namespace daisysp;
using namespace daisy;

static DaisyPod pod;

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
static constexpr int   kMaxModes        = 24;     // most resonators the bank can hold
static constexpr float kBaseFreqHz      = 110.0f; // A2, the lowest partial
static constexpr float kMinT60          = 0.03f;  // Knob 1 range
static constexpr float kMaxT60          = 1.0f;
static constexpr float kFreezeT60       = 60.0f;  // what button 1 holds it at
static constexpr float kRefT60          = 0.3f;   // where makeup gain is unity-ish
static constexpr float kMakeupBase      = 24.0f;  // broadband makeup at kRefT60
static constexpr float kMaxMakeup       = 80.0f;
static constexpr float kStrikeDecay     = 0.0005f;// ~40 ms noise burst
static constexpr float kStrikeLevel     = 0.015f; // strike loudness, after boost
static constexpr float kAgcTarget       = 0.25f;  // wet level the compressor aims for
static constexpr float kAgcGain         = 2.5f;   // how much quiet rings get lifted
static constexpr float kAgcAttack       = 0.002f;
static constexpr float kAgcRelease      = 0.00004f;
static constexpr float kBowLevel        = 0.003f; // held button 2 noise drive
static constexpr float kFreezeGain      = 3.0f;   // lift applied to a frozen ring
static constexpr float kFreezeCoef      = 0.0002f;// ~100 ms swell into freeze
static constexpr float kMaxExcBoost     = 2.0e5f;

// Octaves of the scale the bank is voiced with. Denser tunings use fewer
// octaves, so a chromatic bank ends up one partial per semitone.
static constexpr int   kMaxOctaves      = 3;
static constexpr float kTapMaxMs        = 350.0f;  // button 1 shorter than this = tap
static constexpr float kFreezeHoldMs    = 350.0f;  // longer than this = freeze
static constexpr float kKeyFlashDecay   = 0.0006f; // LED 1 blip on key change
static constexpr float kLevelCoef       = 0.0005f;// LED 1 level follower
static constexpr float kDampingEps      = 0.002f; // knob deadband for retuning

// Scale presets, as semitone offsets from the root
static const int kPentMaj[]  = {0, 2, 4, 7, 9};
static const int kPentMin[]  = {0, 3, 5, 7, 10};
static const int kIonian[]   = {0, 2, 4, 5, 7, 9, 11};
static const int kAeolian[]  = {0, 2, 3, 5, 7, 8, 10};
static const int kDorian[]   = {0, 2, 3, 5, 7, 9, 10};
static const int kPhrygian[] = {0, 1, 3, 5, 7, 8, 10};
static const int kLydian[]   = {0, 2, 4, 6, 7, 9, 11};
static const int kMixolyd[]  = {0, 2, 4, 5, 7, 9, 10};
static const int kLocrian[]  = {0, 1, 3, 5, 6, 8, 10};
static const int kWhole[]    = {0, 2, 4, 6, 8, 10};
static const int kHarmMin[]  = {0, 2, 3, 5, 7, 8, 11};
static const int kChrom[]    = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

struct Preset
{
    const int* deg;
    int        n;
    float      r, g, b;  // LED 2 colour
};

static const Preset kPresets[] = {
    {kPentMaj, 5, 0.0f, 0.8f, 1.0f},    // cyan
    {kPentMin, 5, 0.2f, 1.0f, 0.3f},    // green
    {kIonian, 7, 1.0f, 0.85f, 0.1f},    // yellow
    {kAeolian, 7, 0.25f, 0.4f, 1.0f},   // blue
    {kDorian, 7, 1.0f, 0.45f, 0.05f},   // orange
    {kPhrygian, 7, 0.65f, 0.2f, 1.0f},  // violet
    {kLydian, 7, 1.0f, 0.3f, 0.6f},     // pink
    {kMixolyd, 7, 0.1f, 1.0f, 0.7f},    // teal
    {kLocrian, 7, 1.0f, 0.15f, 0.1f},   // red
    {kWhole, 6, 1.0f, 1.0f, 1.0f},      // white
    {kHarmMin, 7, 1.0f, 0.1f, 0.9f},    // magenta
    {kChrom, 12, 0.6f, 1.0f, 0.1f},     // lime
};
static constexpr int kNumPresets = 12;

// Resonator state
static float freq[kMaxModes];
static float a1[kMaxModes], a2[kMaxModes], g[kMaxModes];
static float amp_l[kMaxModes], amp_r[kMaxModes];
static float z1[kMaxModes], z2[kMaxModes];
static float makeup       = 1.0f;
static float exc_boost    = 1.0f;
static float bow_amp      = 0.1f;
static float freeze_gain  = 1.0f;
static float peak_env     = 0.0f;
static float key_flash    = 0.0f;
static float btn1_held    = 0.0f;
static bool  btn1_prev    = false;
static bool  btn2_prev    = false;
static bool  enc_prev     = false;

static int   preset      = 1;
static int   root         = 0;   // key, in semitones above kBaseFreqHz
static int   active_modes = 21;  // resonators in use at the current tuning
static float damping     = 0.6f;
static float blend       = 0.6f;
static bool  freeze      = false;
static bool  bypass      = false;
static float strike_env  = 0.0f;
static float level       = 0.0f;
static uint32_t rng_state = 0x9E3779B9u;

// std::clamp is C++17, this builds as gnu++14
static float ClampF(float x, float lo, float hi)
{
    if(x < lo)
        return lo;
    if(x > hi)
        return hi;
    return x;
}

// xorshift32, returns -1.0 to 1.0
static float NoiseF()
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (float)(rng_state >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

static float Saturate(float x)
{
    return tanhf(x);
}

// Nearest degree of the scale to a target note
static int SnapToScale(const Preset& p, float target)
{
    int   best    = 0;
    float bestErr = 1e9f;
    for(int o = 0; o < 4; o++)
    {
        for(int d = 0; d < p.n; d++)
        {
            int   s   = p.deg[d] + 12 * o;
            float err = fabsf((float)s - target);
            if(err < bestErr)
            {
                bestErr = err;
                best    = s;
            }
        }
    }
    return best;
}

// Voice the bank as whole octaves of the scale, so every partial is a real
// degree and none of them are filler semitones. Denser tunings get fewer
// octaves, which is what keeps a chromatic bank inside its range.
static void BuildFreqs()
{
    const Preset& p = kPresets[preset];

    int octaves = kMaxModes / p.n;
    if(octaves > kMaxOctaves)
        octaves = kMaxOctaves;
    if(octaves < 1)
        octaves = 1;

    active_modes = p.n * octaves;
    if(active_modes > kMaxModes)
        active_modes = kMaxModes;

    for(int i = 0; i < active_modes; i++)
    {
        int oct   = i / p.n;
        int deg   = i % p.n;
        int semis = p.deg[deg] + 12 * oct + root;

        // A couple of cents of detune stops exact harmonic alignments from
        // becoming hot spots where one note is far louder than its neighbours
        float cents = (NoiseF() * 3.0f) / 1200.0f;
        freq[i]     = kBaseFreqHz * powf(2.0f, (semis + cents) / 12.0f);
    }

    // Spread the partials across the stereo field, low on the left
    for(int i = 0; i < active_modes; i++)
    {
        float pan = -0.7f + 1.4f * (float)i / (float)(active_modes - 1);
        float ang = (pan + 1.0f) * 3.14159265f * 0.25f;
        amp_l[i]  = cosf(ang);
        amp_r[i]  = sinf(ang);
    }

    // Anything past the end of the bank is silent, and starts silent
    for(int i = active_modes; i < kMaxModes; i++)
        z1[i] = z2[i] = 0.0f;
}

// Recompute the resonators. Only called when damping, freeze or scale changes.
static void UpdateModes()
{
    float sr       = pod.AudioSampleRate();
    float knob_t60 = kMinT60 * powf(kMaxT60 / kMinT60, damping);
    float t60      = freeze ? kFreezeT60 : knob_t60;

    for(int i = 0; i < active_modes; i++)
    {
        float f = freq[i];
        if(f > sr * 0.45f)
            f = sr * 0.45f;

        // Upper partials die away a little sooner, like a real body
        float t60i = t60 * powf(freq[0] / f, 0.35f);
        if(t60i < 0.05f)
            t60i = 0.05f;

        float r  = expf(-6.9078f / (t60i * sr));
        float w  = 2.0f * 3.14159265f * f / sr;

        a1[i] = 2.0f * r * cosf(w);
        a2[i] = -r * r;

        // (1 - r^2) sin(w) puts the peak of each resonance at unity
        float taper = 1.0f / (1.0f + 0.08f * (float)i);
        g[i]        = (1.0f - r) * (1.0f + r) * sinf(w) * taper;
    }

    // A narrow resonance only catches a sliver of a broadband signal, so the
    // bank needs makeup gain, and the narrower it gets the more it needs.
    // Taken from the knob, not the freeze time, so freezing doesn't jump level.
    makeup = ClampF(kMakeupBase * sqrtf(knob_t60 / kRefT60), 1.0f, kMaxMakeup);

    // One resonator this narrow only turns a fraction of a burst into ringing,
    // so the strike is boosted by 1/(2(1-r)) to hit the same level whatever the
    // damping is. Bowing is continuous, so it follows the milder sqrt law.
    float r0    = expf(-6.9078f / (t60 * sr));
    float onemr = 1.0f - r0;
    if(onemr < 1e-6f)
        onemr = 1e-6f;
    exc_boost = ClampF(0.5f / onemr, 1.0f, kMaxExcBoost);
    bow_amp   = ClampF(kBowLevel / sqrtf(onemr), 0.01f, 40.0f);
}

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t                                size)
{
    pod.ProcessAnalogControls();
    pod.ProcessDigitalControls();

    // Knob 1 -> damping, Knob 2 -> blend
    float new_damping = pod.knob1.Process();
    blend             = pod.knob2.Process();

    if(fabsf(new_damping - damping) > kDampingEps)
    {
        damping = new_damping;
        UpdateModes();
    }

    // Edges are detected here rather than with RisingEdge/FallingEdge: those
    // stay true for the whole 1 ms debounce window, and this callback runs
    // about twelve times per millisecond, so one press looked like twelve.
    bool b1       = pod.button1.Pressed();
    bool b2       = pod.button2.Pressed();
    bool enc_down = pod.encoder.Pressed();

    bool b1_release = btn1_prev && !b1;
    bool b2_press   = b2 && !btn2_prev;
    bool enc_press  = enc_down && !enc_prev;

    btn1_prev = b1;
    btn2_prev = b2;
    enc_prev  = enc_down;

    // Button 1 -> tap steps the tuning, hold freezes: gate the excitation and
    // stretch the decay out
    if(b1)
        btn1_held = pod.button1.TimeHeldMs();

    if(b1_release)
    {
        if(btn1_held < kTapMaxMs)
        {
            preset = (preset + 1) % kNumPresets;
            BuildFreqs();
            UpdateModes();  // without this the partials keep their old pitches
            key_flash = 1.0f;
        }
        btn1_held = 0.0f;
    }

    bool new_freeze = b1 && btn1_held > kFreezeHoldMs;
    if(new_freeze != freeze)
    {
        freeze = new_freeze;
        UpdateModes();
    }

    // Button 2 -> press to strike, hold to bow with noise
    if(b2_press)
        strike_env = 1.0f;

    bool bow = b2;

    // Encoder turn -> key. The dial is the only control that goes both ways,
    // so it gets the thing you want to step up and down through.
    int32_t inc = pod.encoder.Increment();
    if(inc != 0)
    {
        root = ((root + inc) % 12 + 12) % 12;
        BuildFreqs();
        UpdateModes();
        key_flash = 1.0f;
    }

    if(enc_press)
        bypass = !bypass;

    for(size_t i = 0; i < size; i += 2)
    {
        float inl = in[i];
        float inr = in[i + 1];

        if(bypass)
        {
            out[i]     = inl;
            out[i + 1] = inr;
            continue;
        }

        float exc = freeze ? 0.0f : 0.5f * (inl + inr);

        if(strike_env > 0.0005f)
        {
            exc += NoiseF() * strike_env * kStrikeLevel * exc_boost;
            strike_env -= strike_env * kStrikeDecay;
        }
        else
        {
            strike_env = 0.0f;
        }

        if(bow)
            exc += NoiseF() * bow_amp;

        float sum_l = 0.0f, sum_r = 0.0f;
        for(int m = 0; m < active_modes; m++)
        {
            float y = g[m] * exc + a1[m] * z1[m] + a2[m] * z2[m];
            z2[m]   = z1[m];
            z1[m]   = y;
            sum_l += y * amp_l[m];
            sum_r += y * amp_r[m];
        }

        // Freezing drops the dry signal, so the ring is lifted to sit on its own
        fonepole(freeze_gain, freeze ? kFreezeGain : 1.0f, kFreezeCoef);

        float wl = sum_l * makeup * freeze_gain;
        float wr = sum_r * makeup * freeze_gain;

        // Notes whose harmonics land on several partials can come out many dB
        // hotter than the ones around them, so the wet path gets compressed:
        // loud rings are held near kAgcTarget, quiet ones are lifted by kAgcGain
        float mag = fabsf(wl) + fabsf(wr);
        fonepole(peak_env, mag, mag > peak_env ? kAgcAttack : kAgcRelease);
        float agc = kAgcGain * kAgcTarget / (kAgcTarget + peak_env);

        float resl = Saturate(wl * agc);
        float resr = Saturate(wr * agc);

        // Frozen means you only hear the held ring
        float b = freeze ? 1.0f : blend;
        out[i]  = inl * (1.0f - b) + resl * b;
        out[i + 1] = inr * (1.0f - b) + resr * b;

        fonepole(level, fabsf(resl) + fabsf(resr), kLevelCoef);
        key_flash -= key_flash * kKeyFlashDecay;
    }

    // LED 1 follows how hard the bank is ringing, LED 2 shows the scale
    if(bypass)
    {
        pod.led1.Set(0.0f, 0.0f, 0.0f);
        pod.led2.Set(0.0f, 0.0f, 0.0f);
    }
    else
    {
        // A key change blips LED 1 on top of the ring level
        float lv = ClampF(level * 2.0f, 0.0f, 1.0f);
        if(key_flash > lv)
            lv = key_flash;
        pod.led1.Set(lv * 0.9f, lv * 0.9f, lv);

        const Preset& p = kPresets[preset];
        float         b = freeze ? 1.0f : 0.3f + 0.7f * lv;
        pod.led2.Set(p.r * b, p.g * b, p.b * b);
    }

    pod.UpdateLeds();
}

int main(void)
{
    pod.Init();
    pod.SetAudioBlockSize(4);

    // BuildFreqs lays out the partials and pans them, this only clears state
    for(int i = 0; i < kMaxModes; i++)
        z1[i] = z2[i] = 0.0f;

    rng_state = 0x9E3779B9u ^ System::GetNow();

    BuildFreqs();
    UpdateModes();

    pod.StartAdc();
    pod.StartAudio(AudioCallback);

    while(1) {}
}
