#include "daisysp.h"
#include "daisy_pod.h"

using namespace daisysp;
using namespace daisy;

static DaisyPod pod;

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
static constexpr int   kNumStages   = 6;      // allpass sections in the chain
static constexpr float kSweepMinHz  = 120.0f; // bottom of the sweep
static constexpr float kSweepMaxHz  = 2400.0f;// top of the sweep
static constexpr float kMinRateHz   = 0.05f;  // Knob 1 range
static constexpr float kMaxRateHz   = 6.0f;
static constexpr float kMaxFeedback = 0.92f;  // Knob 2 ceiling
static constexpr float kMix         = 0.5f;   // dry / wet balance
static constexpr float kPi          = 3.14159265f;

enum Mode
{
    MODE_UP = 0,   // endless rise
    MODE_DOWN,     // endless fall
    MODE_BOTH,     // flip direction at the end of every sweep
    MODE_LAST
};

// One allpass chain per channel
static float ax_l[kNumStages], ay_l[kNumStages];
static float ax_r[kNumStages], ay_r[kNumStages];
static float fb_l = 0.0f, fb_r = 0.0f;

static int   mode     = MODE_UP;
static float rate     = 0.6f;
static float feedback = 0.5f;
static float phase    = 0.0f;
static int   dir      = 1;
static float coeff    = 0.0f;
static bool  bypass   = false;
static bool  frozen   = false;
static bool  btn2_prev = false;
static bool  enc_prev  = false;

static float ClampF(float x, float lo, float hi)
{
    if(x < lo)
        return lo;
    if(x > hi)
        return hi;
    return x;
}

static float Saturate(float x)
{
    return tanhf(x);
}

// One first order allpass: y = c*x + x1 - c*y1. Magnitude is flat, only the
// phase moves, which is what makes the notches.
static inline float Allpass(float x, float c, float& x1, float& y1)
{
    float y = c * x + x1 - c * y1;
    x1      = x;
    y1      = y;
    return y;
}

// Run one channel's chain
static float Chain(float in, float c, float* ax, float* ay, float& fb_state)
{
    float v = in + fb_state * feedback;
    for(int i = 0; i < kNumStages; i++)
        v = Allpass(v, c, ax[i], ay[i]);

    fb_state = v;
    return v;
}

static void ResetChain()
{
    for(int i = 0; i < kNumStages; i++)
        ax_l[i] = ay_l[i] = ax_r[i] = ay_r[i] = 0.0f;
    fb_l = fb_r = 0.0f;
}

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t                                size)
{
    pod.ProcessAnalogControls();
    pod.ProcessDigitalControls();

    // Knob 1 -> sweep rate, Knob 2 -> resonance
    rate     = kMinRateHz * powf(kMaxRateHz / kMinRateHz, pod.knob1.Process());
    feedback = pod.knob2.Process() * kMaxFeedback;

    // Edges are detected from the held state: RisingEdge stays true for a whole
    // debounce window, and this callback runs far more often than that
    bool btn2       = pod.button2.Pressed();
    bool enc_down   = pod.encoder.Pressed();
    bool btn2_press = btn2 && !btn2_prev;
    bool enc_press  = enc_down && !enc_prev;
    btn2_prev = btn2;
    enc_prev  = enc_down;

    // Button 1 -> hold to freeze the sweep where it is
    frozen = pod.button1.Pressed();

    // Button 2 -> clear the chain, useful if the feedback runs away
    if(btn2_press)
        ResetChain();

    // Encoder turn -> direction, encoder press -> bypass
    int32_t inc = pod.encoder.Increment();
    if(inc != 0)
        mode = ((mode + inc) % MODE_LAST + MODE_LAST) % MODE_LAST;

    if(enc_press)
        bypass = !bypass;

    float sr = pod.AudioSampleRate();

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

        // Sawtooth sweep: the notches climb (or fall) and snap back at the end
        // of the range. Because the notches are evenly spaced, the snap sounds
        // like the same notch carrying on, which is the barberpole illusion.
        if(!frozen)
        {
            float step = rate / sr;
            phase += dir > 0 ? step : -step;

            if(phase >= 1.0f)
            {
                phase -= 1.0f;
                if(mode == MODE_BOTH)
                    dir = -dir;
            }
            else if(phase < 0.0f)
            {
                phase += 1.0f;
                if(mode == MODE_BOTH)
                    dir = -dir;
            }
        }

        int want_dir = (mode == MODE_DOWN) ? -1 : 1;
        if(mode != MODE_BOTH)
            dir = want_dir;

        float f0 = kSweepMinHz * powf(kSweepMaxHz / kSweepMinHz, phase);

        // The -90 degree point of (c + z^-1)/(1 + c z^-1) sits at
        // cos(w) = -2c/(1 + c^2), so the right coefficient for a notch at f0 is
        // c = -tan(pi/4 - pi*f0/sr). The earlier (1-t)/(1+t) form only moved
        // the phase a fraction of a radian across the whole sweep, which is why
        // there was no audible phasing.
        coeff = -tanf(kPi * 0.25f - kPi * f0 / sr);

        float wl = Chain(inl, coeff, ax_l, ay_l, fb_l);
        float wr = Chain(inr, coeff, ax_r, ay_r, fb_r);

        out[i]     = Saturate(inl * (1.0f - kMix) + wl * kMix);
        out[i + 1] = Saturate(inr * (1.0f - kMix) + wr * kMix);
    }

    // LED 1 tracks the sweep, LED 2 shows the direction
    if(bypass)
    {
        pod.led1.Set(0.0f, 0.0f, 0.0f);
        pod.led2.Set(0.0f, 0.0f, 0.0f);
    }
    else
    {
        float p = phase;
        pod.led1.Set(p * 0.8f, p * 0.8f, p);

        float b = frozen ? 1.0f : 0.4f + 0.6f * p;
        switch(mode)
        {
            case MODE_UP: pod.led2.Set(0.0f, b * 0.9f, b); break;
            case MODE_DOWN: pod.led2.Set(b, b * 0.35f, 0.0f); break;
            default: pod.led2.Set(b * 0.6f, b * 0.2f, b); break;
        }
    }

    pod.UpdateLeds();
}

int main(void)
{
    pod.Init();
    pod.SetAudioBlockSize(4);

    ResetChain();

    pod.StartAdc();
    pod.StartAudio(AudioCallback);

    while(1) {}
}
