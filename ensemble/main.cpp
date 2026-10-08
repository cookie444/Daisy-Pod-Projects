#include "daisysp.h"
#include "daisy_pod.h"

using namespace daisysp;
using namespace daisy;

static DaisyPod pod;

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
static constexpr size_t kMaxDelay    = 2048;   // ~42 ms at 48 kHz
static constexpr float  kBaseDelayMs = 12.0f;  // centre of the modulation
static constexpr float  kMaxDepthMs  = 4.0f;   // Knob 1 at full
static constexpr float  kMinRateHz   = 0.1f;   // Knob 2 range
static constexpr float  kMaxRateHz   = 8.0f;
static constexpr float  kDryWet      = 0.5f;   // default dry / wet balance
static constexpr float  kWidthStep   = 0.05f;  // encoder turn
static constexpr float  kSmoothCoef  = 0.0005f;
static constexpr float  kOutputGain  = 1.10f;  // +10% output
static constexpr float  kPi          = 3.14159265f;
static constexpr float  kDcHz        = 12.0f;  // input DC blocker corner

// Tap tempo (Button 1 short press)
static constexpr float  kDefaultBeatMs = 500.0f;
static constexpr float  kMinBeatMs     = 200.0f;
static constexpr float  kMaxBeatMs     = 1500.0f;
static constexpr float  kTapTimeoutMs  = 3000.0f;
static constexpr uint32_t kModeHoldMs  = 700;    // Button 1 hold -> cycle mode

// Voice bank. Voices are phase-offset and rate-detuned so the ensemble never
// locks into a periodic warble. kPhase is degrees; kPan is -1 (left) to +1.
static constexpr int kNumVoices = 4;
static const float kDetune[kNumVoices] = {1.00f, 1.19f, 1.37f, 1.53f};
static const float kPhase[kNumVoices]  = {0.0f, 120.0f, 240.0f, 180.0f};
static const float kPan[kNumVoices]    = {-1.0f, 0.0f, 1.0f, 1.0f};

// BBD-era character
static constexpr bool  kEnableCompander = true;
static constexpr float kEmphGain        = 0.35f;    // pre / de-emphasis depth
static constexpr float kPreEmphHz       = 1000.0f;  // compander corner
static constexpr float kReconHz         = 12000.0f; // reconstruction low-pass

enum Mode
{
    MODE_ENSEMBLE = 0, // Solina swirl
    MODE_CHORUS,       // tighter Juno-style
    MODE_VIBRATO,      // wet-only pitch wobble
    MODE_LAST
};

struct ModeCfg
{
    uint8_t mask;       // which voices are active
    float   rate_scale; // LFO rate multiplier
};

static const ModeCfg kModes[MODE_LAST] = {
    {0x7, 1.0f}, // ENSEMBLE — voices 0/1/2 at 0/120/240 degrees
    {0x9, 1.6f}, // CHORUS   — voices 0/3 at 0/180 degrees
    {0x1, 1.0f}, // VIBRATO  — voice 0 only
};

static DelayLine<float, kMaxDelay> delay_l, delay_r;

static int   mode   = MODE_ENSEMBLE;
static float depth  = 0.5f; // 0..1
static float rate   = 0.5f; // Hz
static float width  = 0.7f; // 0..1 stereo spread
static bool  bypass = false;

static float phase[kNumVoices];

// Filter state
static float pre_lp_l = 0.0f, pre_lp_r = 0.0f;
static float de_lp_l  = 0.0f, de_lp_r  = 0.0f;
static float recon_l  = 0.0f, recon_r  = 0.0f;
static float dc_lp_l  = 0.0f, dc_lp_r  = 0.0f;
static float mix_smooth = kDryWet;

static bool btn1_prev = false;
static bool enc_prev  = false;

// Tap tempo state
static float    tempo_ms         = kDefaultBeatMs;
static bool     synced           = false;
static uint32_t last_tap_ms      = 0;
static float    tap_intervals[4] = {0.0f, 0.0f, 0.0f, 0.0f};
static int      tap_count        = 0;
static bool     first_tap        = true;
static float    knob2_last       = -1.0f;

static uint32_t b1_press_ms = 0;
static bool     mode_fired  = false;

// ---------------------------------------------------------------------------
// Record / cassette "bump" (Button 2). During a bump the whole signal fades to
// 100% wet and its playback speed is knocked around: a lurch that springs back
// plus a wow/flutter wobble. Fresh random values every press. No added tone.
// ---------------------------------------------------------------------------
static constexpr float kBumpWetCoef      = 0.0025f; // ~8 ms crossfade to full wet
static constexpr float kMaxOffsetSamples = 260.0f;  // clamp the accumulated lurch

static bool  bump_active  = false;
static float bump_t       = 0.0f;   // seconds since trigger
static float bump_len     = 0.30f;  // seconds
static float bump_speed   = 0.04f;  // peak playback-speed deviation
static float bump_dir     = 1.0f;   // knock up or down
static float bump_cycles  = 1.0f;   // how many times the speed wobbles
static float bump_wob_hz  = 8.0f;   // wow / flutter rate
static float bump_wob_amt = 15.0f;  // wow / flutter depth, in samples
static float bump_dropout = 0.10f;  // gain dip depth
static float bump_phase   = 0.0f;   // random start phase of the speed wobble
static float bump_offset  = 0.0f;   // accumulated delay offset, in samples
static float bump_wet     = 0.0f;   // 0..1 crossfade toward full wet

static uint32_t rng_state = 0x9E3779B9u;

static float ClampF(float x, float lo, float hi)
{
    if(x < lo)
        return lo;
    if(x > hi)
        return hi;
    return x;
}

static float RandF()
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (float)(rng_state >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

static float Rand01()
{
    return RandF() * 0.5f + 0.5f;
}

static void TriggerBump()
{
    bump_active  = true;
    bump_t       = 0.0f;
    bump_offset  = 0.0f;
    bump_len     = 0.15f + Rand01() * 0.45f;         // 150-600 ms
    bump_speed   = 0.020f + Rand01() * 0.070f;       // 2-9 % peak speed
    bump_dir     = (Rand01() < 0.5f) ? -1.0f : 1.0f;
    bump_cycles  = 1.0f + floorf(Rand01() * 3.0f);   // 1-3 speed wobbles
    bump_wob_hz  = 4.0f + Rand01() * 16.0f;          // 4-20 Hz warble
    bump_wob_amt = 4.0f + Rand01() * 32.0f;          // 4-36 samples
    bump_dropout = Rand01() * 0.30f;                 // 0-30 % dip
    bump_phase   = RandF() * kPi;
}

static void RegisterTap(uint32_t now)
{
    if(first_tap)
    {
        first_tap   = false;
        tap_count   = 0;
        last_tap_ms = now;
        return;
    }

    uint32_t delta = now - last_tap_ms;

    if(delta > (uint32_t)kTapTimeoutMs)
    {
        tap_count   = 0;
        last_tap_ms = now;
    }
    else if(delta > 50)
    {
        if(tap_count < 4)
            tap_intervals[tap_count++] = (float)delta;
        else
        {
            tap_intervals[0] = tap_intervals[1];
            tap_intervals[1] = tap_intervals[2];
            tap_intervals[2] = tap_intervals[3];
            tap_intervals[3] = (float)delta;
        }

        float avg = 0.0f;
        for(int i = 0; i < tap_count; i++)
            avg += tap_intervals[i];

        tempo_ms    = ClampF(avg / (float)tap_count, kMinBeatMs, kMaxBeatMs);
        last_tap_ms = now;
        synced      = true;
    }
}

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t                                size)
{
    pod.ProcessAnalogControls();
    pod.ProcessDigitalControls();

    // Knob 1 -> depth, Knob 2 -> base rate
    depth  = pod.knob1.Process();
    float k2 = pod.knob2.Process();
    rate   = kMinRateHz * powf(kMaxRateHz / kMinRateHz, k2);

    // Moving Knob 2 takes the rate back from tap tempo
    if(synced && fabsf(k2 - knob2_last) > 0.01f)
        synced = false;
    knob2_last = k2;

    uint32_t now = System::GetNow();

    // Button 1: short press -> tap tempo, hold -> cycle mode
    bool b1        = pod.button1.Pressed();
    bool enc_down  = pod.encoder.Pressed();
    bool enc_press = enc_down && !enc_prev;

    if(b1 && !btn1_prev)
    {
        b1_press_ms = now;
        mode_fired  = false;
    }
    if(b1 && !mode_fired && (now - b1_press_ms) > kModeHoldMs)
    {
        mode       = (mode + 1) % MODE_LAST;
        mode_fired = true;
    }
    if(!b1 && btn1_prev)
    {
        if(!mode_fired)
            RegisterTap(now);
    }
    btn1_prev = b1;
    enc_prev  = enc_down;

    // Button 2: random record / cassette bump
    static bool btn2_prev = false;
    bool b2 = pod.button2.Pressed();
    if(b2 && !btn2_prev)
        TriggerBump();
    btn2_prev = b2;

    // Encoder turn -> stereo width, encoder press -> bypass
    int32_t inc = pod.encoder.Increment();
    if(inc != 0)
        width = ClampF(width + (float)inc * kWidthStep, 0.0f, 1.0f);

    if(enc_press)
        bypass = !bypass;

    const ModeCfg& cfg = kModes[mode];

    int active = 0;
    for(int v = 0; v < kNumVoices; v++)
        if(cfg.mask & (1 << v))
            active++;
    float norm = (active > 0) ? 1.0f / (float)active : 1.0f;

    // A single voice has nowhere to pan, so force it centre
    float pan_width = (active > 1) ? width : 0.0f;

    // Equal-power pan gains, computed once per block instead of per sample
    float pan_l[kNumVoices];
    float pan_r[kNumVoices];
    for(int v = 0; v < kNumVoices; v++)
    {
        float th = (kPan[v] * pan_width + 1.0f) * (kPi * 0.25f);
        pan_l[v] = cosf(th);
        pan_r[v] = sinf(th);
    }

    // VIBRATO is wet only
    float target = (mode == MODE_VIBRATO) ? 1.0f : kDryWet;
    fonepole(mix_smooth, target, kSmoothCoef);

    float sr         = pod.AudioSampleRate();
    float base       = kBaseDelayMs * 0.001f * sr;
    float depth_s    = depth * kMaxDepthMs * 0.001f * sr;
    float base_rate  = synced ? (1000.0f / tempo_ms) : rate;
    float hz         = base_rate * cfg.rate_scale;
    float inv_sr     = 1.0f / sr;
    float coef_pre   = 1.0f - expf(-2.0f * kPi * kPreEmphHz / sr);
    float coef_recon = 1.0f - expf(-2.0f * kPi * kReconHz / sr);
    float coef_dc    = 1.0f - expf(-2.0f * kPi * kDcHz / sr);

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

        // Input DC blocker (keeps DC out of the emphasised delay path)
        fonepole(dc_lp_l, inl, coef_dc);
        fonepole(dc_lp_r, inr, coef_dc);
        inl -= dc_lp_l;
        inr -= dc_lp_r;

        // Advance the bump: accumulate a playback-speed lurch that springs back,
        // plus a wow/flutter wobble, and crossfade toward full wet.
        float bump_warp = 0.0f;
        float bump_gain = 1.0f;
        if(bump_active)
        {
            bump_t += inv_sr;
            float u = bump_t / bump_len;
            if(u >= 1.0f)
            {
                bump_active = false;
            }
            else
            {
                float env = sinf(kPi * u);                    // 0 at both ends
                float osc = sinf(2.0f * kPi * bump_cycles * u + bump_phase);
                bump_offset += bump_dir * bump_speed * env * osc;
                bump_offset = ClampF(bump_offset, -kMaxOffsetSamples,
                                     kMaxOffsetSamples);
                float wob = bump_wob_amt * env
                            * sinf(2.0f * kPi * bump_wob_hz * bump_t);
                bump_warp  = bump_offset + wob;
                bump_gain  = 1.0f - bump_dropout * env;
            }
        }
        fonepole(bump_wet, bump_active ? 1.0f : 0.0f, kBumpWetCoef);
        float warp    = bump_warp * bump_wet;
        float mix_eff = mix_smooth + (1.0f - mix_smooth) * bump_wet;

        // Pre-emphasis (encode): boost highs before the delay line
        float el = inl;
        float er = inr;
        if(kEnableCompander)
        {
            float hp_l = inl - pre_lp_l;
            float hp_r = inr - pre_lp_r;
            fonepole(pre_lp_l, inl, coef_pre);
            fonepole(pre_lp_r, inr, coef_pre);
            el = inl + kEmphGain * hp_l;
            er = inr + kEmphGain * hp_r;
        }

        delay_l.Write(el);
        delay_r.Write(er);

        float wet_l = 0.0f;
        float wet_r = 0.0f;

        for(int v = 0; v < kNumVoices; v++)
        {
            if(!(cfg.mask & (1 << v)))
                continue;

            phase[v] += hz * kDetune[v] * inv_sr;
            if(phase[v] >= 1.0f)
                phase[v] -= 1.0f;

            float lfo = sinf(2.0f * kPi * phase[v]);
            float d   = ClampF(base + lfo * depth_s + warp,
                               1.0f, (float)(kMaxDelay - 2));

            wet_l += delay_l.ReadHermite(d) * pan_l[v];
            wet_r += delay_r.ReadHermite(d) * pan_r[v];
        }
        wet_l *= norm;
        wet_r *= norm;

        // De-emphasis (decode): undo the encode on the wet path
        if(kEnableCompander)
        {
            float hd_l = wet_l - de_lp_l;
            float hd_r = wet_r - de_lp_r;
            fonepole(de_lp_l, wet_l, coef_pre);
            fonepole(de_lp_r, wet_r, coef_pre);
            wet_l -= kEmphGain * hd_l;
            wet_r -= kEmphGain * hd_r;
        }

        // Reconstruction low-pass
        fonepole(recon_l, wet_l, coef_recon);
        fonepole(recon_r, wet_r, coef_recon);
        wet_l = recon_l;
        wet_r = recon_r;

        float yl = (inl * (1.0f - mix_eff) + wet_l * mix_eff) * bump_gain;
        float yr = (inr * (1.0f - mix_eff) + wet_r * mix_eff) * bump_gain;

        out[i]     = yl * kOutputGain;
        out[i + 1] = yr * kOutputGain;
    }

    // LED 1 follows the modulation (flashes on a bump), LED 2 shows the mode.
    // Everything is pink.
    if(bypass)
    {
        pod.led1.Set(0.0f, 0.0f, 0.0f);
        pod.led2.Set(0.0f, 0.0f, 0.0f);
    }
    else if(bump_active)
    {
        pod.led1.Set(1.0f, 0.45f, 0.75f);
        pod.led2.Set(1.0f, 0.45f, 0.75f);
    }
    else
    {
        float m  = fabsf(sinf(2.0f * kPi * phase[0]));
        float br = 0.25f + 0.75f * m;
        pod.led1.Set(br, br * 0.35f, br * 0.7f);

        switch(mode)
        {
            case MODE_ENSEMBLE: pod.led2.Set(1.0f, 0.60f, 0.80f); break; // soft pink
            case MODE_CHORUS: pod.led2.Set(0.9f, 0.15f, 0.55f); break;   // magenta
            default: pod.led2.Set(1.0f, 0.40f, 0.45f); break;            // rose
        }
    }

    pod.UpdateLeds();
}

int main(void)
{
    pod.Init();
    pod.SetAudioBlockSize(4);

    delay_l.Init();
    delay_r.Init();

    for(int v = 0; v < kNumVoices; v++)
        phase[v] = kPhase[v] / 360.0f;

    rng_state = 0x9E3779B9u ^ System::GetNow();

    pod.StartAdc();
    pod.StartAudio(AudioCallback);

    while(1) {}
}
