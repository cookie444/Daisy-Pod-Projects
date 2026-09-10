#include "daisysp.h"
#include "daisy_pod.h"

using namespace daisysp;
using namespace daisy;

static DaisyPod pod;

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
static constexpr size_t kMaxDelay      = 1024;  // ~21 ms at 48 kHz
static constexpr float  kBaseDelayMs   = 10.0f; // centre of the wobble
static constexpr float  kMaxDepthMs    = 6.0f;  // Knob 1 at full
static constexpr float  kStereoOffsetMs = 2.5f; // static spread between channels
static constexpr float  kMinRateHz     = 0.1f;  // Knob 2 range
static constexpr float  kMaxRateHz     = 8.0f;
static constexpr float  kMix           = 0.5f;  // dry / wet balance
static constexpr float  kBrakeCoef     = 0.0005f;
static constexpr float  kRandomGlide   = 0.0008f;
static constexpr float  kDefaultBeatMs = 500.0f;
static constexpr float  kMinBeatMs     = 200.0f;
static constexpr float  kMaxBeatMs     = 1500.0f;
static constexpr float  kTapTimeoutMs  = 3000.0f;
static constexpr float  kTempoHoldMs   = 1000.0f;
static constexpr float  kPi            = 3.14159265f;

enum Wave
{
    WAVE_SINE = 0,  // smooth wow
    WAVE_TRI,       // symmetrical flutter
    WAVE_RANDOM,    // tape drift, glides between random levels
    WAVE_SH,        // stepped random, the damage tape actually does
    WAVE_LAST
};

static DelayLine<float, kMaxDelay> delay_l, delay_r;

static int   wave      = WAVE_SINE;
static float depth     = 1.0f;    // ms
static float rate      = 1.0f;    // Hz
static float phase     = 0.0f;
static float rnd_target = 0.0f;
static float rnd_value  = 0.0f;
static float brake     = 1.0f;
static bool  bypass    = false;
static uint32_t rng_state = 0x9E3779B9u;

// Tap tempo
static float    tempo_ms         = kDefaultBeatMs;
static bool     synced           = false;
static uint32_t last_tap_ms      = 0;
static float    tap_intervals[4] = {0.0f, 0.0f, 0.0f, 0.0f};
static int      tap_count        = 0;
static bool     first_tap        = true;

static bool btn1_prev = false;
static bool enc_prev  = false;

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

static float WaveAt(float ph)
{
    switch(wave)
    {
        case WAVE_TRI: return 4.0f * fabsf(ph - 0.5f) - 1.0f;
        case WAVE_RANDOM:
        case WAVE_SH: return rnd_value;
        default: return sinf(2.0f * kPi * ph);
    }
}

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t                                size)
{
    pod.ProcessAnalogControls();
    pod.ProcessDigitalControls();

    // Knob 1 -> depth, Knob 2 -> rate
    depth = pod.knob1.Process() * kMaxDepthMs;
    rate  = kMinRateHz * powf(kMaxRateHz / kMinRateHz, pod.knob2.Process());

    // Edges are detected from the held state: RisingEdge stays true for a whole
    // debounce window, and this callback runs far more often than that
    bool b1        = pod.button1.Pressed();
    bool enc_down  = pod.encoder.Pressed();
    bool b1_press  = b1 && !btn1_prev;
    bool enc_press = enc_down && !enc_prev;
    btn1_prev = b1;
    enc_prev  = enc_down;

    // Button 1 -> tap tempo syncs the wobble to one cycle per beat,
    // hold to go back to the knob
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
                synced      = true;
            }
        }
    }
    if(b1 && pod.button1.TimeHeldMs() > kTempoHoldMs)
    {
        synced    = false;
        first_tap = true;
        tap_count = 0;
    }

    // Button 2 -> hold to brake the wobble to a stop
    fonepole(brake, pod.button2.Pressed() ? 0.0f : 1.0f, kBrakeCoef);

    // Encoder turn -> waveform, encoder press -> bypass
    int32_t inc = pod.encoder.Increment();
    if(inc != 0)
        wave = ((wave + inc) % WAVE_LAST + WAVE_LAST) % WAVE_LAST;

    if(enc_press)
        bypass = !bypass;

    float sr        = pod.AudioSampleRate();
    float base      = kBaseDelayMs * 0.001f * sr;
    float spread    = kStereoOffsetMs * 0.001f * sr;
    float depth_s   = depth * 0.001f * sr;
    float hz        = synced ? (1000.0f / tempo_ms) : rate;
    float phase_inc = hz * brake / sr;

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

        phase += phase_inc;
        if(phase >= 1.0f)
        {
            phase -= 1.0f;
            rnd_target = RandF();
            if(wave == WAVE_SH)
                rnd_value = rnd_target;
        }

        // Tape drift glides between random levels instead of stepping
        fonepole(rnd_value, rnd_target, kRandomGlide);

        float ml = WaveAt(phase);
        float mr = WaveAt(phase < 0.5f ? phase + 0.5f : phase - 0.5f);

        float dl = ClampF(base + ml * depth_s, 1.0f, (float)(kMaxDelay - 2));
        float dr = ClampF(base + spread + mr * depth_s, 1.0f, (float)(kMaxDelay - 2));

        float wl = delay_l.ReadHermite(dl);
        float wr = delay_r.ReadHermite(dr);

        delay_l.Write(inl);
        delay_r.Write(inr);

        out[i]     = inl * (1.0f - kMix) + wl * kMix;
        out[i + 1] = inr * (1.0f - kMix) + wr * kMix;
    }

    // LED 1 follows the wobble, LED 2 shows the waveform
    if(bypass)
    {
        pod.led1.Set(0.0f, 0.0f, 0.0f);
        pod.led2.Set(0.0f, 0.0f, 0.0f);
    }
    else
    {
        float m  = fabsf(WaveAt(phase));
        float br = 0.25f + 0.75f * m;
        pod.led1.Set(br * 0.8f, br * 0.8f, br);

        switch(wave)
        {
            case WAVE_SINE: pod.led2.Set(0.0f, 0.8f, 1.0f); break;
            case WAVE_TRI: pod.led2.Set(0.2f, 1.0f, 0.3f); break;
            case WAVE_RANDOM: pod.led2.Set(1.0f, 0.7f, 0.1f); break;
            default: pod.led2.Set(1.0f, 0.2f, 0.4f); break;
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

    rng_state = 0x9E3779B9u ^ System::GetNow();

    pod.StartAdc();
    pod.StartAudio(AudioCallback);

    while(1) {}
}
