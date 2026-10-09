#include "daisysp.h"
#include "daisy_pod.h"

using namespace daisysp;
using namespace daisy;

static DaisyPod pod;

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
static constexpr float kPi              = 3.14159265f;
static constexpr float kOutputLevel     = 0.8f;

// Pitch detection: decimate the input, run YIN-style autocorrelation on it.
static constexpr int   kDecim           = 4;                 // 48k -> 12k
static constexpr float kDecimSr         = 48000.0f / kDecim;
static constexpr int   kPitchWindow     = 384;               // decimated samples
static constexpr int   kLagMin          = 12;                // ~1000 Hz
static constexpr int   kLagMax          = 200;               // ~60 Hz
static constexpr int   kHop             = 32;                // decimated samples between runs
static constexpr float kYinThresh       = 0.18f;

// Gate (note-on / note-off from input amplitude)
static constexpr float kInGain          = 3.0f;              // input gain into the follower
static constexpr float kGateOn          = 0.020f;            // on the gained follower
static constexpr float kGateOff         = 0.010f;
static constexpr float kEnvAttack       = 0.05f;             // follower attack coef
static constexpr float kEnvRelease      = 0.0010f;           // follower release coef

// Envelopes
static constexpr float kVcaAttack       = 0.0120f;           // ~1.7 ms
static constexpr float kVcaRelease      = 0.00020f;          // ~100 ms
static constexpr float kFiltDecay       = 0.00015f;          // ~140 ms
static constexpr float kPitchSlew       = 0.0150f;           // ~1.4 ms portamento

// Filter
static constexpr float kBaseCutMin      = 400.0f;
static constexpr float kBaseCutMax      = 14000.0f;
static constexpr float kSweepAmount     = 5.0f;
static constexpr float kKeyTrackRef     = 110.0f;  // key-tracking reference (A2)
static constexpr float kKeyTrackAmt     = 0.4f;    // 0 = none, 1 = full

enum Wave
{
    WAVE_SAW = 0,
    WAVE_SQUARE,
    WAVE_SUB,
    WAVE_SAW_SUB,
    WAVE_LAST
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static int   wave      = WAVE_SAW;
static int   octave    = 0;      // -1, 0, +1
static float cutoff_k  = 0.6f;   // 0..1 knob
static float res_k     = 0.3f;   // 0..1 knob
static bool  bypass    = false;
static bool  freeze    = false;

static bool  btn1_prev = false;
static bool  enc_prev  = false;
static bool  btn2_prev = false;

// Input / gate
static float in_follow = 0.0f;
static bool  gate      = false;
static bool  prev_gate = false;

// Pitch detection
static float pitch_buf[kPitchWindow];
static int   pitch_w    = 0;
static int   decim_cnt  = 0;
static int   hop_cnt    = 0;
static float dec_lp1    = 0.0f;
static float dec_lp2    = 0.0f;
static float detected_hz = 0.0f;
static bool  voiced      = false;
static bool  snap_pitch  = false;

// Voice
static float osc_freq   = 110.0f;
static float target_freq = 110.0f;
static float osc_phase  = 0.0f;
static float sub_phase  = 0.0f;
static float vca_env    = 0.0f;
static float filt_env   = 0.0f;

// TPT state-variable filter
static float svf_ic1 = 0.0f, svf_ic2 = 0.0f;
static float svf_a1 = 0.0f, svf_a2 = 0.0f, svf_a3 = 0.0f;

static float ClampF(float x, float lo, float hi)
{
    if(x < lo)
        return lo;
    if(x > hi)
        return hi;
    return x;
}

// Band-limited step for PolyBLEP oscillators
static float PolyBLEP(float t, float dt)
{
    if(t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if(t > 1.0f - dt)
    {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

// YIN-style pitch detection over the decimated ring buffer
static void DetectPitch()
{
    static float lin[kPitchWindow];
    static float d[kLagMax + 2];
    static float cm[kLagMax + 2];

    for(int i = 0; i < kPitchWindow; i++)
        lin[i] = pitch_buf[(pitch_w + i) % kPitchWindow];

    for(int tau = kLagMin; tau <= kLagMax + 1; tau++)
    {
        float sum = 0.0f;
        int   n   = kPitchWindow - tau;
        for(int i = 0; i < n; i++)
        {
            float diff = lin[i] - lin[i + tau];
            sum += diff * diff;
        }
        d[tau] = sum;
    }

    float run = 0.0f;
    for(int tau = kLagMin; tau <= kLagMax + 1; tau++)
    {
        run += d[tau];
        cm[tau] = (run > 0.0f)
                      ? d[tau] * (float)(tau - kLagMin + 1) / run
                      : 1.0f;
    }

    int best = -1;
    for(int tau = kLagMin + 1; tau <= kLagMax; tau++)
    {
        if(cm[tau] < kYinThresh && cm[tau] <= cm[tau - 1]
           && cm[tau] < cm[tau + 1])
        {
            best = tau;
            break;
        }
    }

    if(best < 0)
    {
        voiced = false;
        return;
    }

    float y0    = cm[best - 1];
    float y1    = cm[best];
    float y2    = cm[best + 1];
    float denom = y0 - 2.0f * y1 + y2;
    float shift = (fabsf(denom) > 1e-9f) ? 0.5f * (y0 - y2) / denom : 0.0f;
    float tau_f = (float)best + shift;

    if(tau_f < 1.0f)
    {
        voiced = false;
        return;
    }

    detected_hz = kDecimSr / tau_f;
    voiced      = true;
}

// Push one decimated sample into the pitch buffer
static void PushPitchSample(float s)
{
    pitch_buf[pitch_w] = s;
    pitch_w            = (pitch_w + 1) % kPitchWindow;

    if(++hop_cnt >= kHop)
    {
        hop_cnt = 0;
        DetectPitch();
    }
}

static void HsvToRgb(float h, float s, float v, float& r, float& g, float& b)
{
    float i = floorf(h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - f * s);
    float t = v * (1.0f - (1.0f - f) * s);
    switch((int)i % 6)
    {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
}

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t                                size)
{
    pod.ProcessAnalogControls();
    pod.ProcessDigitalControls();

    // Knob 1 -> cutoff, Knob 2 -> resonance
    cutoff_k = pod.knob1.Process();
    res_k    = pod.knob2.Process();

    bool b1       = pod.button1.Pressed();
    bool b2       = pod.button2.Pressed();
    bool enc_down = pod.encoder.Pressed();
    bool enc_press = enc_down && !enc_prev;

    // Button 1 -> octave cycle
    if(b1 && !btn1_prev)
    {
        octave++;
        if(octave > 1)
            octave = -1;
    }
    // Button 2 -> hold to freeze (drone the current note)
    freeze = b2;

    // Encoder turn -> waveform, press -> bypass
    int32_t inc = pod.encoder.Increment();
    if(inc != 0)
        wave = ((wave + inc) % WAVE_LAST + WAVE_LAST) % WAVE_LAST;
    if(enc_press)
        bypass = !bypass;

    btn1_prev = b1;
    btn2_prev = b2;
    enc_prev  = enc_down;

    float sr          = pod.AudioSampleRate();
    float base_cut    = kBaseCutMin * powf(kBaseCutMax / kBaseCutMin, cutoff_k);
    float q           = 0.5f + res_k * 7.5f;
    float oct_mult    = powf(2.0f, (float)octave);
    float k_dec       = 1.0f - expf(-2.0f * kPi * 3800.0f / sr); // anti-alias for decim
    float dec_inc     = 1.0f / (float)kDecim;

    for(size_t i = 0; i < size; i += 2)
    {
        float dry_l = in[i];
        float dry_r = in[i + 1];

        if(bypass)
        {
            out[i]     = dry_l;
            out[i + 1] = dry_r;
            continue;
        }

        float mono = 0.5f * (dry_l + dry_r);

        // ---- input follower + gate ----
        float rect = fabsf(mono) * kInGain;
        float coef = (rect > in_follow) ? kEnvAttack : kEnvRelease;
        in_follow += coef * (rect - in_follow);

        prev_gate = gate;
        if(in_follow > kGateOn)
            gate = true;
        else if(in_follow < kGateOff)
            gate = false;

        bool trigger = gate && !prev_gate;
        if(trigger)
            snap_pitch = true;

        // ---- decimate for pitch ----
        fonepole(dec_lp1, mono, k_dec);
        fonepole(dec_lp2, dec_lp1, k_dec);
        decim_cnt++;
        if(decim_cnt >= kDecim)
        {
            decim_cnt = 0;
            PushPitchSample(dec_lp2);
        }

        // ---- pitch target ----
        if(!freeze)
        {
            if(voiced)
            {
                if(target_freq < 1.0f || snap_pitch)
                {
                    target_freq = detected_hz;
                    snap_pitch  = false;
                }
                else
                {
                    target_freq += 0.55f * (detected_hz - target_freq);
                }
            }
        }
        else if(target_freq < 1.0f)
        {
            target_freq = 110.0f;
        }

        float goal_freq = ClampF(target_freq * oct_mult, 20.0f, 5000.0f);
        osc_freq += kPitchSlew * (goal_freq - osc_freq);

        // ---- envelopes ----
        bool held = gate || freeze;
        if(held)
            vca_env += kVcaAttack * (1.0f - vca_env);
        else
            vca_env += kVcaRelease * (0.0f - vca_env);

        if(trigger || (freeze && !prev_gate))
            filt_env = 1.0f;
        else
            filt_env += kFiltDecay * (0.0f - filt_env);

        // ---- oscillators ----
        float dt = osc_freq / sr;
        osc_phase += dt;
        if(osc_phase >= 1.0f)
            osc_phase -= 1.0f;
        sub_phase += dt * 0.5f;
        if(sub_phase >= 1.0f)
            sub_phase -= 1.0f;

        float saw   = 2.0f * osc_phase - 1.0f - PolyBLEP(osc_phase, dt);
        float sq    = (osc_phase < 0.5f) ? 1.0f : -1.0f;
        sq += PolyBLEP(osc_phase, dt);
        sq -= PolyBLEP((osc_phase < 0.5f) ? osc_phase + 0.5f
                                          : osc_phase - 0.5f,
                       dt);
        float sub = (sub_phase < 0.5f) ? 1.0f : -1.0f;

        float osc;
        switch(wave)
        {
            case WAVE_SQUARE: osc = sq; break;
            case WAVE_SUB: osc = sub; break;
            case WAVE_SAW_SUB: osc = 0.65f * saw + 0.65f * sub; break;
            default: osc = saw; break;
        }

        // ---- filter (TPT SVF low-pass); key-tracked, per-sample coefficients ----
        float keytrack = powf(osc_freq / kKeyTrackRef, kKeyTrackAmt);
        keytrack       = ClampF(keytrack, 0.25f, 4.0f);
        float fc       = base_cut * keytrack * (1.0f + kSweepAmount * filt_env);
        fc             = ClampF(fc, 30.0f, 0.45f * sr);
        {
            float g  = tanf(kPi * fc / sr);
            float kk = 1.0f / q;
            svf_a1   = 1.0f / (1.0f + g * (g + kk));
            svf_a2   = g * svf_a1;
            svf_a3   = g * svf_a2;
        }

        float v3 = osc - svf_ic2;
        float v1 = svf_a1 * svf_ic1 + svf_a2 * v3;
        float v2 = svf_ic2 + svf_a2 * svf_ic1 + svf_a3 * v3;
        svf_ic1  = 2.0f * v1 - svf_ic1;
        svf_ic2  = 2.0f * v2 - svf_ic2;
        float lp = v2;

        float y = lp * vca_env * kOutputLevel;
        out[i]     = y;
        out[i + 1] = y;
    }

    // ---- LEDs ----
    if(bypass)
    {
        pod.led1.Set(0.0f, 0.0f, 0.0f);
        pod.led2.Set(0.0f, 0.0f, 0.0f);
    }
    else
    {
        float br = 0.15f + 0.85f * vca_env;
        float r, g, b;
        if(voiced && detected_hz > 20.0f)
        {
            float midi = 69.0f + 12.0f * log2f(detected_hz / 440.0f);
            float pc   = midi - 12.0f * floorf(midi / 12.0f); // pitch class 0..12
            HsvToRgb(pc / 12.0f, 0.9f, br, r, g, b);
        }
        else
        {
            r = g = b = br * 0.6f;
        }
        pod.led1.Set(r, g, b);

        switch(wave)
        {
            case WAVE_SAW: pod.led2.Set(1.0f, 0.6f, 0.1f); break;
            case WAVE_SQUARE: pod.led2.Set(0.2f, 0.8f, 1.0f); break;
            case WAVE_SUB: pod.led2.Set(0.5f, 0.3f, 1.0f); break;
            default: pod.led2.Set(1.0f, 0.3f, 0.6f); break;
        }
    }

    pod.UpdateLeds();
}

int main(void)
{
    pod.Init();
    pod.SetAudioBlockSize(48);

    for(int i = 0; i < kPitchWindow; i++)
        pitch_buf[i] = 0.0f;

    pod.StartAdc();
    pod.StartAudio(AudioCallback);

    while(1) {}
}
