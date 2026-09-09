#include "daisysp.h"
#include "daisy_pod.h"

using namespace daisysp;
using namespace daisy;

static DaisyPod pod;

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
static constexpr size_t kBufferSamples   = 49152;  // ~1.02 s at 48 kHz
static constexpr float  kMinSliceSamples = 32.0f;
static constexpr float  kFadeCoef        = 0.02f;  // wet crossfade, ~1 ms
static constexpr float  kWetGain         = 0.9f;   // repeats sit just under the dry
static constexpr float  kWetToneHz       = 6000.0f;// repeats are darker than the dry
static constexpr float  kWetSpreadMs     = 7.0f;   // L/R offset on repeats
static constexpr float  kFlashDecay      = 0.0012f;// LED 1 decay, per sample

// Tempo. Always on, 120 BPM until you tap something else.
static constexpr float  kDefaultBeatMs   = 500.0f;
static constexpr float  kMinBeatMs       = 200.0f; // 300 BPM
static constexpr float  kMaxBeatMs       = 1000.0f;// 60 BPM
static constexpr float  kTapTimeoutMs    = 3000.0f;
static constexpr float  kTempoHoldMs     = 1000.0f;// hold button 1 to reset to 120

// Knob 1 picks a division of the beat, so slices always land on the grid
static constexpr float kDivisions[]  = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f,  6.0f,
                                        8.0f, 12.0f, 16.0f, 24.0f, 32.0f,
                                        48.0f};
static constexpr int   kNumDivisions = 12;

// Trigger pattern: a 16-step Euclidean rhythm, hits spread as evenly as
// possible. This is what makes it groove instead of sounding random.
static constexpr int kPatternSteps = 16;

// PITCH mode walks up a whole tone per repeat, stopping at the octave
static constexpr float kPitchStepSemis = 2.0f;

enum Mode
{
    MODE_CHOP = 0,
    MODE_STUTTER,
    MODE_PITCH,
    MODE_REVERSE,
    MODE_LAST
};

// Four ~1 s buffers, in external SDRAM
static float DSY_SDRAM_BSS rec_l[kBufferSamples];
static float DSY_SDRAM_BSS rec_r[kBufferSamples];
static float DSY_SDRAM_BSS frz_l[kBufferSamples];
static float DSY_SDRAM_BSS frz_r[kBufferSamples];

// Engine state
static size_t write_idx        = 0;
static size_t slice_len        = 2400;
static size_t slice_len_target = 2400;
static size_t slice_pos        = 0;
static float  read_pos         = 0.0f;
static float  rate             = 1.0f;
static int    repeats_left     = 0;
static int    repeat_index     = 0;
static bool   active           = false;
static float  wet              = 0.0f;
static float  wet_target       = 0.0f;
static float  chaos            = 0.0f;
static bool   force            = false;
static bool   force_once       = false;
static int    mode             = MODE_STUTTER;
static bool   bypass           = false;
static float  flash            = 0.0f;
static float  spread_samples   = 0.0f;
static uint32_t rng_state      = 0x9E3779B9u;

// Trigger pattern
static bool pattern[kPatternSteps];
static int  pattern_step = 0;
static int  pattern_hits = -1;

// Wet path tone
static OnePole lp_l, lp_r;

// Tap tempo
static float    tempo_ms      = kDefaultBeatMs;
static uint32_t last_tap_ms   = 0;
static float    tap_intervals[4] = {0.0f, 0.0f, 0.0f, 0.0f};
static int      tap_count     = 0;
static bool     first_tap     = true;

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

// Bresenham-style even spread of `hits` across the 16 steps. Step 0 is always
// a hit when there is at least one, so the downbeat never moves.
static void BuildPattern(int hits)
{
    int acc = 0;
    for(int i = 0; i < kPatternSteps; i++)
    {
        acc += hits;
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

static float ReadFrz(const float* buf, float pos)
{
    size_t i0 = (size_t)pos;
    float  fr = pos - (float)i0;
    size_t i1 = i0 + 1;
    if(i1 >= slice_len)
        i1 -= slice_len;
    float a = buf[i0];
    float b = buf[i1];
    return a + fr * (b - a);
}

// Playback rate for the next repeat in a sequence
static float NextRate()
{
    if(mode == MODE_REVERSE)
    {
        // Backwards by default, occasionally flips forward at high chaos
        return (RandF() < 0.25f * chaos) ? 1.0f : -1.0f;
    }
    if(mode == MODE_PITCH)
    {
        float semis = kPitchStepSemis * (float)repeat_index;
        return ClampF(powf(2.0f, semis / 12.0f), 1.0f, 2.0f);
    }
    return 1.0f;
}

// Mostly two repeats, sometimes three, rarely four
static int NextRepeats()
{
    int reps = 2;
    if(RandF() < 0.4f)
        reps = 3;
    if(RandF() < chaos * 0.25f)
        reps = 4;
    return reps;
}

// Freeze the slice that just ended. write_idx points at the next slot, so the
// slice lives at [write_idx - slice_len, write_idx).
static void CaptureSlice()
{
    size_t len = slice_len;
    size_t src = (write_idx + kBufferSamples - len) % kBufferSamples;
    for(size_t i = 0; i < len; i++)
    {
        frz_l[i] = rec_l[src];
        frz_r[i] = rec_r[src];
        if(++src >= kBufferSamples)
            src = 0;
    }
}

static void StartSequence(int reps)
{
    repeats_left = reps;
    repeat_index = 0;
    rate         = (mode == MODE_REVERSE) ? -1.0f : 1.0f;
    read_pos     = rate >= 0.0f ? 0.0f : (float)(slice_len - 1);
    active       = true;
    wet_target   = 1.0f;
}

// Called every time a slice boundary is crossed
static void OnSliceBoundary()
{
    // Length changes land on the grid. A running sequence would otherwise index
    // past the captured audio, so it is dropped here instead.
    if(slice_len_target != slice_len)
    {
        slice_len  = slice_len_target;
        active     = false;
        wet_target = 0.0f;
    }

    bool hit = pattern[pattern_step];
    pattern_step = (pattern_step + 1) % kPatternSteps;

    if(active)
    {
        // One repeat per slice until the sequence runs out
        repeats_left--;
        if(repeats_left <= 0)
        {
            active     = false;
            wet_target = 0.0f;
        }
        else
        {
            repeat_index++;
            rate     = NextRate();
            read_pos = rate >= 0.0f ? 0.0f : (float)(slice_len - 1);
        }
    }
    else if(force || force_once)
    {
        // Button 2: manual trigger, always stutters whatever the mode
        CaptureSlice();
        StartSequence(2 + (int)(RandF() * 3.0f));
        force_once = false;
    }
    else if(mode == MODE_CHOP)
    {
        // Euclidean gate: mute on the hits
        wet_target = hit ? 1.0f : 0.0f;
    }
    else if(hit)
    {
        CaptureSlice();
        StartSequence(NextRepeats());
    }
    else
    {
        wet_target = 0.0f;
    }

    flash = hit ? 1.0f : 0.3f;
}

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t                                size)
{
    float inl, inr;

    pod.ProcessAnalogControls();
    pod.ProcessDigitalControls();

    // Knob 1 -> division of the beat, Knob 2 -> chaos (pattern density)
    float div_knob = pod.knob1.Process();
    chaos          = pod.knob2.Process();

    int div_idx = (int)(div_knob * (float)(kNumDivisions - 1) + 0.5f);
    if(div_idx < 0)
        div_idx = 0;
    if(div_idx >= kNumDivisions)
        div_idx = kNumDivisions - 1;

    float slice_ms = tempo_ms / kDivisions[div_idx];
    slice_len_target = (size_t)ClampF(
        slice_ms * 0.001f * pod.AudioSampleRate(),
        kMinSliceSamples,
        (float)kBufferSamples);

    // Rebuild the pattern only when the density changes, so the groove stays
    // put while the knob is untouched
    int hits = (int)(chaos * (float)kPatternSteps + 0.5f);
    if(hits != pattern_hits)
    {
        pattern_hits = hits;
        BuildPattern(hits);
    }

    // Right channel reads a little further into the frozen slice, which widens
    // the repeats without a second delay line
    spread_samples = fminf(kWetSpreadMs * 0.001f * pod.AudioSampleRate(),
                           (float)(slice_len - 1));

    // Button 1 -> tap tempo, hold to go back to 120 BPM
    if(pod.button1.RisingEdge())
    {
        uint32_t now = System::GetNow();

        if(first_tap)
        {
            // The first tap only anchors the reference, there is no interval yet
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

                tempo_ms = ClampF(avg / (float)tap_count, kMinBeatMs, kMaxBeatMs);
                last_tap_ms = now;
            }
            // Intervals of 50ms or less are switch bounce, ignore them
        }
    }
    if(pod.button1.Pressed() && pod.button1.TimeHeldMs() > kTempoHoldMs)
    {
        tempo_ms  = kDefaultBeatMs;
        first_tap = true;
        tap_count = 0;
    }

    // Button 2 -> trigger. Press for one burst, hold for continuous stutter.
    force = pod.button2.Pressed();
    if(pod.button2.RisingEdge())
    {
        // Cut the current slice short so the burst starts immediately
        force_once = true;
        slice_pos  = slice_len;
    }

    // Encoder turn -> mode, encoder press -> bypass
    int32_t inc = pod.encoder.Increment();
    if(inc != 0)
        mode = ((mode + inc) % MODE_LAST + MODE_LAST) % MODE_LAST;

    if(pod.encoder.RisingEdge())
        bypass = !bypass;

    // The only way to switch the effect off is the encoder press, so turning the
    // dial can never land you on silence by accident
    bool off = bypass;

    for(size_t i = 0; i < size; i += 2)
    {
        inl = in[i];
        inr = in[i + 1];

        if(off)
        {
            // Keep recording so engaging mid-phrase is seamless
            rec_l[write_idx] = inl;
            rec_r[write_idx] = inr;
            out[i]           = inl;
            out[i + 1]       = inr;

            if(++write_idx >= kBufferSamples)
                write_idx = 0;
            continue;
        }

        rec_l[write_idx] = inl;
        rec_r[write_idx] = inr;

        if(slice_pos >= slice_len)
        {
            OnSliceBoundary();
            slice_pos = 0;
        }

        float wl = 0.0f, wr = 0.0f;
        if(active)
        {
            wl = ReadFrz(frz_l, read_pos);

            // Right channel: same slice, offset by a few ms
            float pr = read_pos + spread_samples;
            if(pr >= (float)slice_len)
                pr -= (float)slice_len;
            wr = ReadFrz(frz_r, pr);

            read_pos += rate;
            if(read_pos >= (float)slice_len)
                read_pos -= (float)slice_len;
            else if(read_pos < 0.0f)
                read_pos += (float)slice_len;

            // Darker and a touch quieter than the dry signal
            wl = lp_l.Process(wl) * kWetGain;
            wr = lp_r.Process(wr) * kWetGain;
        }

        // Short crossfade keeps the slice edges from clicking
        fonepole(wet, wet_target, kFadeCoef);

        float dry_gain = 1.0f - wet;
        out[i]         = inl * dry_gain + wl * wet;
        out[i + 1]     = inr * dry_gain + wr * wet;

        flash -= flash * kFlashDecay;

        if(++write_idx >= kBufferSamples)
            write_idx = 0;
        slice_pos++;
    }

    // LED 1 flashes on every slice, brighter on pattern hits.
    // LED 2 shows the mode and lights with the wet crossfade.
    if(off)
    {
        pod.led1.Set(0.0f, 0.0f, 0.0f);
        pod.led2.Set(0.0f, 0.0f, 0.0f);
    }
    else
    {
        float f = flash;
        pod.led1.Set(f * 0.9f, f * 0.9f, f);

        float b = 0.2f + 0.8f * wet;
        switch(mode)
        {
            case MODE_CHOP: pod.led2.Set(b, b * 0.25f, 0.0f); break;
            case MODE_STUTTER: pod.led2.Set(0.0f, b * 0.8f, b); break;
            case MODE_PITCH: pod.led2.Set(b * 0.6f, b * 0.2f, b); break;
            case MODE_REVERSE: pod.led2.Set(b * 0.2f, b, b * 0.3f); break;
            default: break;
        }
    }

    pod.UpdateLeds();
}

int main(void)
{
    pod.Init();
    pod.SetAudioBlockSize(4);

    float sample_rate = pod.AudioSampleRate();

    // The .sdram_bss section is not guaranteed to come up cleared
    for(size_t i = 0; i < kBufferSamples; i++)
    {
        rec_l[i] = rec_r[i] = 0.0f;
        frz_l[i] = frz_r[i] = 0.0f;
    }

    // One-pole cutoff is normalized: cutoff_hz / sample_rate
    lp_l.Init();
    lp_r.Init();
    lp_l.SetFrequency(kWetToneHz / sample_rate);
    lp_r.SetFrequency(kWetToneHz / sample_rate);

    rng_state = 0x9E3779B9u ^ System::GetNow();
    BuildPattern(0);

    slice_len = slice_len_target = (size_t)(0.125f * sample_rate);

    pod.StartAdc();
    pod.StartAudio(AudioCallback);

    while(1) {}
}
