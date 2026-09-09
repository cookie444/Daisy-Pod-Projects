#include "daisysp.h"
#include "daisy_pod.h"

#define MAX_DELAY static_cast<size_t>(48000 * 2.0f)

using namespace daisysp;
using namespace daisy;

static DaisyPod pod;

// Delay lines (stored in external SDRAM)
static DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS delay_l;
static DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS delay_r;

// One-pole lowpass filters for analog warmth (feedback path only)
static OnePole fl, fr;

// Tape wobble modulation LFO
static Oscillator lfo;

// State
static float current_delay, delay_target;
static float feedback_val, mix_val;
static bool  effect_on = true;
static bool  mod_on    = false;

// Delay time in ms, shared by tap tempo and encoder
static float tempo_ms = 500.0f;

// Tap tempo
static uint32_t last_tap_ms = 0;
static float    tap_intervals[4] = {0.0f, 0.0f, 0.0f, 0.0f};
static int      tap_count   = 0;
static bool     first_tap   = true;

// Tuning
static constexpr float kMinDelayMs     = 20.0f;
static constexpr float kMaxDelayMs     = 2000.0f;
static constexpr float kEncoderStepMs  = 5.0f;
static constexpr float kFilterCutoffHz = 4500.0f;
static constexpr float kModRateHz      = 1.1f;
static constexpr float kModDepthMs     = 1.5f;
static constexpr float kMaxFeedback    = 0.92f;

static float mod_depth_samples = 0.0f;

// Analog saturation (own name to avoid conflict with daisysp::SoftClip)
static float AnalogSat(float x)
{
    if(x > 1.0f)
        return 1.0f;
    if(x < -1.0f)
        return -1.0f;
    return x - (x * x * x) / 3.0f;
}

// std::clamp is C++17, this builds as gnu++14
static float ClampF(float x, float lo, float hi)
{
    if(x < lo)
        return lo;
    if(x > hi)
        return hi;
    return x;
}

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t                                size)
{
    float outl, outr, inl, inr;

    pod.ProcessAnalogControls();
    pod.ProcessDigitalControls();

    // Knob 1 -> Feedback, Knob 2 -> Mix
    feedback_val = pod.knob1.Process() * kMaxFeedback;
    mix_val      = pod.knob2.Process();

    // Button 1 -> Bypass toggle
    if(pod.button1.RisingEdge())
    {
        effect_on = !effect_on;
        fl.Reset();
        fr.Reset();
    }

    // Button 2 -> Tap tempo
    if(pod.button2.RisingEdge())
    {
        uint32_t now = System::GetNow();

        if(first_tap)
        {
            // First tap only anchors the reference, there is no interval yet
            first_tap   = false;
            tap_count   = 0;
            last_tap_ms = now;
        }
        else
        {
            uint32_t delta = now - last_tap_ms;

            if(delta > 3000)
            {
                // Paused too long, start a fresh run
                tap_count   = 0;
                last_tap_ms = now;
            }
            else if(delta > 50)
            {
                // Keep the four most recent intervals
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

                tempo_ms = ClampF(avg / (float)tap_count,
                                  kMinDelayMs,
                                  kMaxDelayMs);

                last_tap_ms = now;
            }
            // Intervals of 50ms or less are switch bounce, ignore them
        }
    }

    // Encoder turn -> manual delay time (fine-tunes what tap tempo set)
    int32_t inc = pod.encoder.Increment();
    if(inc != 0)
    {
        tempo_ms = ClampF(tempo_ms + (float)inc * kEncoderStepMs,
                          kMinDelayMs,
                          kMaxDelayMs);
    }

    // Encoder press -> toggle modulation
    if(pod.encoder.RisingEdge())
        mod_on = !mod_on;

    delay_target = tempo_ms * 0.001f * pod.AudioSampleRate();

    // LED1 green = effect on
    // LED2 blinks at tempo: red = modulation off, blue = modulation on
    pod.led1.Set(0.0f, effect_on ? 1.0f : 0.0f, 0.0f);

    if(effect_on)
    {
        static uint32_t blink_counter = 0;
        // size counts interleaved floats, so frames (samples per channel)
        // is size / 2. Using size directly ran the clock at 2x and halved
        // the blink period.
        blink_counter += size / 2;
        uint32_t period = (uint32_t)(tempo_ms * 0.001f * pod.AudioSampleRate());
        bool     blink  = (blink_counter % period)
                         < (uint32_t)(pod.AudioSampleRate() * 0.05f);
        if(mod_on)
            pod.led2.Set(0.0f, 0.0f, blink ? 1.0f : 0.0f);
        else
            pod.led2.Set(blink ? 1.0f : 0.0f, 0.0f, 0.0f);
    }
    else
    {
        pod.led2.Set(0.0f, 0.0f, 0.0f);
    }

    pod.UpdateLeds();

    for(size_t i = 0; i < size; i += 2)
    {
        inl = in[i];
        inr = in[i + 1];

        if(effect_on)
        {
            // Smooth base delay time for click-free tap/encoder changes
            fonepole(current_delay, delay_target, 0.00007f);

            // Modulation rides on top of the smoothed time
            float mod = mod_on ? lfo.Process() * mod_depth_samples : 0.0f;
            float d = ClampF(current_delay + mod, 1.0f, (float)(MAX_DELAY - 2));

            // Bright read: this is what you hear
            outl = delay_l.ReadHermite(d);
            outr = delay_r.ReadHermite(d);

            // Filter + saturate only what is recirculated, so the first repeat
            // stays bright and later repeats darken progressively
            float fbl = AnalogSat(fl.Process(outl));
            float fbr = AnalogSat(fr.Process(outr));

            delay_l.Write(inl + feedback_val * fbl);
            delay_r.Write(inr + feedback_val * fbr);

            // Dry/wet mix
            out[i]     = inl * (1.0f - mix_val) + outl * mix_val;
            out[i + 1] = inr * (1.0f - mix_val) + outr * mix_val;
        }
        else
        {
            delay_l.Write(inl);
            delay_r.Write(inr);
            out[i]     = inl;
            out[i + 1] = inr;
        }
    }
}

int main(void)
{
    float sample_rate;

    pod.Init();
    pod.SetAudioBlockSize(4);
    sample_rate = pod.AudioSampleRate();

    delay_l.Init();
    delay_r.Init();

    // One-pole cutoff is normalized: cutoff_hz / sample_rate
    fl.Init();
    fr.Init();
    fl.SetFrequency(kFilterCutoffHz / sample_rate);
    fr.SetFrequency(kFilterCutoffHz / sample_rate);

    // Tape wobble LFO
    lfo.Init(sample_rate);
    lfo.SetFreq(kModRateHz);
    lfo.SetAmp(1.0f);
    lfo.SetWaveform(Oscillator::WAVE_TRI);
    mod_depth_samples = kModDepthMs * 0.001f * sample_rate;

    current_delay = delay_target = sample_rate * 0.5f;
    delay_l.SetDelay(current_delay);
    delay_r.SetDelay(current_delay);

    pod.StartAdc();
    pod.StartAudio(AudioCallback);

    while(1) {}
}
