#include "daisysp.h"
#include "daisy_pod.h"

using namespace daisysp;
using namespace daisy;

static DaisyPod pod;

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
static constexpr int   kPatternSteps  = 16;     // steps per bar, sixteenth notes
static constexpr int   kStepsPerBeat  = 4;      // so the bar is four beats
static constexpr float kMinCutoffHz   = 250.0f; // where the filter closes to
static constexpr float kMaxCutoffHz   = 8000.0f;// ceiling of Knob 1
static constexpr float kClosedLevel   = 0.35f;  // how far a non-hit step opens
static constexpr float kResonance     = 0.6f;   // filter resonance
static constexpr float kGlideCoef     = 0.0015f;// cutoff glide, ~3 ms
static constexpr float kFlashDecay    = 0.0012f;
static constexpr float kDefaultBeatMs = 500.0f; // 120 BPM
static constexpr float kMinBeatMs     = 200.0f;
static constexpr float kMaxBeatMs     = 1500.0f;
static constexpr float kTapTimeoutMs  = 3000.0f;
static constexpr float kTempoHoldMs   = 1000.0f;

enum Shape
{
    SHAPE_GATE = 0,  // flat, the pattern is everything
    SHAPE_RAMP_UP,
    SHAPE_RAMP_DOWN,
    SHAPE_TRIANGLE,
    SHAPE_RANDOM,
    SHAPE_LAST
};

static Svf filt_l, filt_r;

// Engine state
static bool  pattern[kPatternSteps];
static float contour[kPatternSteps];
static int   step           = 0;
static size_t step_len      = 2400;
static size_t step_pos      = 0;
static int   shape          = SHAPE_GATE;
static int   hits           = -1;
static float density        = 0.0f;
static float cutoff_hi      = 4000.0f;
static float cutoff         = kMinCutoffHz;
static float cutoff_target  = kMinCutoffHz;
static bool  bypass         = false;
static bool  latch          = false;
static float flash          = 0.0f;
static uint32_t rng_state   = 0x9E3779B9u;

// Tap tempo
static float    tempo_ms         = kDefaultBeatMs;
static uint32_t last_tap_ms      = 0;
static float    tap_intervals[4] = {0.0f, 0.0f, 0.0f, 0.0f};
static int      tap_count        = 0;
static bool     first_tap        = true;

static bool btn1_prev = false;
static bool enc_prev  = false;

// std::clamp is C++17, this builds as gnu++14
static float ClampF(float x, float lo, float hi)
{
    if(x < lo)
        return lo;
    if(x > hi)
        return hi;
    return x;
}

// xorshift32, returns 0.0 - 1.0
static float RandF()
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (float)(rng_state >> 8) * (1.0f / 16777216.0f);
}

// Bresenham-style even spread of `n` hits across the 16 steps. Step 0 is always
// a hit when there is at least one, so the downbeat never moves.
static void BuildPattern(int n)
{
    int acc = 0;
    for(int i = 0; i < kPatternSteps; i++)
    {
        acc += n;
        if(acc >= kPatternSteps)
        {
            acc -= kPatternSteps;
            pattern[i] = true;
        }
        else
        {
            pattern[i] = false;
        }
    }
}

// The underlying shape the filter follows between hits
static void BuildContour()
{
    for(int i = 0; i < kPatternSteps; i++)
    {
        float t = (float)i / (float)(kPatternSteps - 1);
        switch(shape)
        {
            case SHAPE_RAMP_UP: contour[i] = t; break;
            case SHAPE_RAMP_DOWN: contour[i] = 1.0f - t; break;
            case SHAPE_TRIANGLE: contour[i] = 1.0f - fabsf(2.0f * t - 1.0f); break;
            case SHAPE_RANDOM: contour[i] = RandF(); break;
            default: contour[i] = 0.0f; break;
        }
    }
}

// Cutoff for the step we just landed on
static void UpdateStep()
{
    float v = pattern[step] ? 1.0f : kClosedLevel * contour[step];
    cutoff_target = kMinCutoffHz * powf(cutoff_hi / kMinCutoffHz, v);
    flash         = pattern[step] ? 1.0f : 0.3f;

    step++;
    if(step >= kPatternSteps)
    {
        step = 0;
        if(shape == SHAPE_RANDOM)
            BuildContour();  // new contour every bar
    }
}

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t                                size)
{
    pod.ProcessAnalogControls();
    pod.ProcessDigitalControls();

    // Knob 1 -> how far the pattern opens, Knob 2 -> pattern density
    float hi_knob = pod.knob1.Process();
    density       = pod.knob2.Process();

    cutoff_hi = kMinCutoffHz * powf(kMaxCutoffHz / kMinCutoffHz, hi_knob);

    int want_hits = (int)(density * (float)kPatternSteps + 0.5f);
    if(want_hits != hits)
    {
        hits = want_hits;
        BuildPattern(hits);
    }

    float step_ms = tempo_ms / (float)kStepsPerBeat;
    step_len      = (size_t)(step_ms * 0.001f * pod.AudioSampleRate());
    if(step_len < 32)
        step_len = 32;

    // Edges are detected from the held state: RisingEdge stays true for a whole
    // debounce window, and this callback runs far more often than that
    bool b1       = pod.button1.Pressed();
    bool enc_down = pod.encoder.Pressed();
    bool b1_press = b1 && !btn1_prev;
    bool enc_press = enc_down && !enc_prev;
    btn1_prev = b1;
    enc_prev  = enc_down;

    // Button 1 -> tap tempo, hold to go back to 120 BPM
    if(b1_press)
    {
        uint32_t now = System::GetNow();

        if(first_tap)
        {
            first_tap   = false;
            tap_count   = 0;
            last_tap_ms = now;
        }
        else
        {
            uint32_t delta = now - last_tap_ms;

            if(delta > kTapTimeoutMs)
            {
                tap_count   = 0;
                last_tap_ms = now;
            }
            else if(delta > 50)
            {
                if(tap_count < 4)
                {
                    tap_intervals[tap_count++] = (float)delta;
                }
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
            }
        }
    }
    if(b1 && pod.button1.TimeHeldMs() > kTempoHoldMs)
    {
        tempo_ms  = kDefaultBeatMs;
        first_tap = true;
        tap_count = 0;
    }

    // Button 2 -> hold to latch the filter wherever it is
    latch = pod.button2.Pressed();

    // Encoder turn -> shape, encoder press -> bypass
    int32_t inc = pod.encoder.Increment();
    if(inc != 0)
    {
        shape = ((shape + inc) % SHAPE_LAST + SHAPE_LAST) % SHAPE_LAST;
        BuildContour();
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

        if(step_pos >= step_len)
        {
            UpdateStep();
            step_pos = 0;
        }

        if(!latch)
            fonepole(cutoff, cutoff_target, kGlideCoef);

        filt_l.SetFreq(cutoff);
        filt_r.SetFreq(cutoff);
        filt_l.Process(inl);
        filt_r.Process(inr);

        out[i]     = filt_l.Low();
        out[i + 1] = filt_r.Low();

        flash -= flash * kFlashDecay;
        step_pos++;
    }

    // LED 1 flashes per step, LED 2 shows the shape
    if(bypass)
    {
        pod.led1.Set(0.0f, 0.0f, 0.0f);
        pod.led2.Set(0.0f, 0.0f, 0.0f);
    }
    else
    {
        float f = flash;
        pod.led1.Set(f * 0.9f, f * 0.9f, f);

        float b = latch ? 1.0f : 0.35f + 0.65f * f;
        switch(shape)
        {
            case SHAPE_GATE: pod.led2.Set(0.0f, b * 0.8f, b); break;
            case SHAPE_RAMP_UP: pod.led2.Set(b * 0.2f, b, b * 0.3f); break;
            case SHAPE_RAMP_DOWN: pod.led2.Set(b, b * 0.6f, 0.0f); break;
            case SHAPE_TRIANGLE: pod.led2.Set(b * 0.6f, b * 0.2f, b); break;
            default: pod.led2.Set(b, b * 0.25f, 0.0f); break;
        }
    }

    pod.UpdateLeds();
}

int main(void)
{
    pod.Init();
    pod.SetAudioBlockSize(4);

    float sample_rate = pod.AudioSampleRate();

    filt_l.Init(sample_rate);
    filt_r.Init(sample_rate);
    filt_l.SetRes(kResonance);
    filt_r.SetRes(kResonance);

    rng_state = 0x9E3779B9u ^ System::GetNow();
    BuildContour();
    BuildPattern(0);
    cutoff = cutoff_target = kMinCutoffHz;

    pod.StartAdc();
    pod.StartAudio(AudioCallback);

    while(1) {}
}
