#include "daisysp.h"
#include "daisy_pod.h"

using namespace daisysp;
using namespace daisy;

static DaisyPod pod;

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
static constexpr int   kNumVowels   = 5;
static constexpr int   kNumFormants = 3;
static constexpr float kFormantQ    = 0.82f;  // Svf resonance, 0-1
static constexpr float kF1Gain      = 1.0f;
static constexpr float kF2Gain      = 0.6f;
static constexpr float kF3Gain      = 0.25f;
static constexpr float kOutputTrim  = 0.7f;
static constexpr float kPronounceMs = 120.0f; // length of the per-beat blip
static constexpr float kExcDrive    = 0.08f;  // noise burst into the formants
static constexpr float kArticMin    = 0.15f;  // floor so the beat always plays
static constexpr float kArticMax    = 1.0f;
static constexpr float kBlinkDuty   = 0.4f;   // LED on fraction of each beat
static constexpr float kFlashDecay  = 0.0012f;
static constexpr float kDefaultBeatMs = 500.0f;  // 120 BPM
static constexpr float kMinBeatMs   = 200.0f;
static constexpr float kMaxBeatMs   = 1500.0f;
static constexpr float kTapTimeoutMs = 3000.0f;
static constexpr float kTempoHoldMs  = 1000.0f;

// Formant centres in Hz: F1, F2, F3
static const float kVowels[kNumVowels][kNumFormants] = {
    {730.0f, 1090.0f, 2440.0f},  // A
    {530.0f, 1840.0f, 2480.0f},  // E
    {270.0f, 2290.0f, 3010.0f},  // I
    {570.0f, 840.0f, 2410.0f},   // O
    {300.0f, 870.0f, 2240.0f},   // U
};

// LED 2 colour per vowel
static const float kVowelColor[kNumVowels][3] = {
    {1.0f, 0.15f, 0.1f},   // A red
    {1.0f, 0.45f, 0.05f},  // E orange
    {1.0f, 0.9f, 0.1f},    // I yellow
    {0.2f, 1.0f, 0.3f},    // O green
    {0.25f, 0.4f, 1.0f},   // U blue
};

static Svf form_l[kNumFormants];
static Svf form_r[kNumFormants];

static int   vowel        = 0;    // the encoder picks which one you hear
static float articulation = 1.0f; // Knob 1, how hard each beat pronounces
static float pronounce    = 0.0f; // per-beat articulation envelope
static float mix          = 0.8f;
static bool  bypass       = false;
static float level        = 0.0f;
static float flash        = 0.0f;

// Beat clock. Four plosives per beat, the LED blinks once per beat.
static constexpr int   kSubsPerBeat = 4;
static size_t beat_len = 2400;
static size_t beat_pos = 0;
static size_t sub_len  = 600;
static size_t sub_pos  = 0;
static bool   double_time = false;

// Tap tempo
static float    tempo_ms         = kDefaultBeatMs;
static uint32_t last_tap_ms      = 0;
static float    tap_intervals[4] = {0.0f, 0.0f, 0.0f, 0.0f};
static int      tap_count        = 0;
static bool     first_tap        = true;

static bool btn1_prev = false;
static bool btn2_prev = false;
static bool enc_prev  = false;

static uint32_t rng_state = 0x9E3779B9u;

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

static void UpdateFormants()
{
    const float* v = kVowels[vowel];
    for(int f = 0; f < kNumFormants; f++)
    {
        float hz = v[f];
        if(hz > 16000.0f)
            hz = 16000.0f;

        form_l[f].SetFreq(hz);
        form_r[f].SetFreq(hz);
    }
}

// One beat landed: re-pronounce the vowel
static void Pronounce()
{
    pronounce = 1.0f;
    flash     = 1.0f;
}

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t                                size)
{
    pod.ProcessAnalogControls();
    pod.ProcessDigitalControls();

    // Knob 1 -> how hard each beat pronounces, Knob 2 -> mix. The articulation
    // is floored so the beat is audible even with the knob at zero
    float knob_a    = pod.knob1.Process();
    articulation    = kArticMin + knob_a * (kArticMax - kArticMin);
    mix             = pod.knob2.Process();

    // Edges are detected from the held state: RisingEdge stays true for a whole
    // debounce window, and this callback runs far more often than that
    bool b1        = pod.button1.Pressed();
    bool b2        = pod.button2.Pressed();
    bool enc_down  = pod.encoder.Pressed();
    bool b1_press  = b1 && !btn1_prev;
    bool b2_press  = b2 && !btn2_prev;
    bool enc_press = enc_down && !enc_prev;
    btn1_prev = b1;
    btn2_prev = b2;
    enc_prev  = enc_down;

    // Button 1 -> tap tempo, one beat per tap, like 16Jobs. Hold to reset.
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

                tempo_ms = ClampF(avg / (float)tap_count, kMinBeatMs, kMaxBeatMs);
                last_tap_ms = now;
            }
        }

        // Every tap fires the plosive right away and re-syncs the grid, so the
        // audio answers the press and the quarter-note beat follows your taps
        beat_pos = 0;
        sub_pos  = 0;
        Pronounce();
    }
    if(b1 && pod.button1.TimeHeldMs() > kTempoHoldMs)
    {
        tempo_ms  = kDefaultBeatMs;
        first_tap = true;
        tap_count = 0;
    }

    // Button 2 -> toggle double time: eight plosives per beat instead of four
    if(b2_press)
        double_time = !double_time;

    // Encoder turn -> live vowel, encoder press -> bypass
    int32_t inc = pod.encoder.Increment();
    if(inc != 0)
    {
        vowel = ((vowel + inc) % kNumVowels + kNumVowels) % kNumVowels;
        UpdateFormants();
        Pronounce();
    }

    if(enc_press)
        bypass = !bypass;

    beat_len = (size_t)(tempo_ms * 0.001f * pod.AudioSampleRate());
    if(beat_len < 32)
        beat_len = 32;
    sub_len = beat_len / (double_time ? kSubsPerBeat * 2 : kSubsPerBeat);
    if(sub_len < 8)
        sub_len = 8;

    float pronounce_decay = 1.0f / (kPronounceMs * 0.001f * pod.AudioSampleRate());

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

        if(++sub_pos >= sub_len)
        {
            Pronounce();  // four plosives per beat, evenly spaced
            sub_pos = 0;
        }

        // Articulation blip: spikes at the beat and decays
        pronounce -= pronounce * pronounce_decay;
        if(pronounce < 0.0f)
            pronounce = 0.0f;

        // The blip also fires a short noise burst into the formants, so the
        // vowel is actually re-excited and speaks rather than just getting a
        // level bump
        float exc = pronounce * articulation * kExcDrive;
        float xl  = inl + NoiseF() * exc;
        float xr  = inr + NoiseF() * exc;

        float wl = 0.0f, wr = 0.0f;
        for(int f = 0; f < kNumFormants; f++)
        {
            form_l[f].Process(xl);
            form_r[f].Process(xr);

            float gain = (f == 0) ? kF1Gain : ((f == 1) ? kF2Gain : kF3Gain);
            wl += form_l[f].Band() * gain;
            wr += form_r[f].Band() * gain;
        }

        wl *= kOutputTrim;
        wr *= kOutputTrim;

        // The pronounce envelope lifts the wet level on each beat
        float lift = 1.0f + pronounce * articulation * 1.2f;
        wl *= lift;
        wr *= lift;

        out[i]     = inl * (1.0f - mix) + wl * mix;
        out[i + 1] = inr * (1.0f - mix) + wr * mix;

        fonepole(level, fabsf(wl) + fabsf(wr), 0.0005f);
        flash -= flash * kFlashDecay;

        if(++beat_pos >= beat_len)
            beat_pos = 0;
    }

    // LED 1 blinks at the tapped tempo, on for a slice of each beat, so the
    // beat is visible at a glance like a tap-tempo LED. LED 2 shows the vowel.
    if(bypass)
    {
        pod.led1.Set(0.0f, 0.0f, 0.0f);
        pod.led2.Set(0.0f, 0.0f, 0.0f);
    }
    else
    {
        bool on = beat_pos < (size_t)(beat_len * kBlinkDuty);
        pod.led1.Set(on ? 1.0f : 0.0f, on ? 1.0f : 0.0f, on ? 1.0f : 0.0f);

        float b = 0.6f;
        pod.led2.Set(kVowelColor[vowel][0] * b,
                     kVowelColor[vowel][1] * b,
                     kVowelColor[vowel][2] * b);
    }

    pod.UpdateLeds();
}

int main(void)
{
    pod.Init();
    pod.SetAudioBlockSize(4);

    float sample_rate = pod.AudioSampleRate();

    for(int f = 0; f < kNumFormants; f++)
    {
        form_l[f].Init(sample_rate);
        form_r[f].Init(sample_rate);
        form_l[f].SetRes(kFormantQ);
        form_r[f].SetRes(kFormantQ);
    }

    UpdateFormants();

    pod.StartAdc();
    pod.StartAudio(AudioCallback);

    while(1) {}
}