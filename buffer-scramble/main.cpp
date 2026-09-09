#include "daisysp.h"
#include "daisy_pod.h"

using namespace daisysp;
using namespace daisy;

static DaisyPod pod;

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
static constexpr int   kMaxWindowSlices   = 8;      // most slices in a loop
static constexpr int   kMinWindowSlices   = 2;      // fewest, once slices get long
static constexpr size_t kMaxSliceSamples  = 48000;  // ~1 s at 48 kHz
static constexpr size_t kMinSliceSamples  = 128;
static constexpr size_t kMaxWindowSamples = 96000;  // ~2 s, caps the latency
static constexpr size_t kBufferSamples    = kMaxWindowSlices * kMaxSliceSamples + 4096;
static constexpr size_t kFadeSamples      = 48;     // ~1 ms slice edge fade
static constexpr float  kFlashDecay       = 0.0012f;// LED 1 decay, per sample
static constexpr float  kWindowFlashDecay = 0.0004f;// LED 2 pulse decay

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

// Knob 2 at full does this many random swaps of the slice order
static constexpr float kMaxSwaps = 12.0f;

enum Mode
{
    MODE_SCRAMBLE = 0,  // shuffled order, replayed forward
    MODE_REVERSE,       // shuffled order, read backwards
    MODE_PINGPONG,      // alternate direction every window
    MODE_DRIFT,         // rotate the order one slot every window
    MODE_LAST
};

// One circular buffer per channel, in external SDRAM. The read head always sits
// one window behind the write head, which is what makes tempo changes instant.
static float DSY_SDRAM_BSS buf_l[kBufferSamples];
static float DSY_SDRAM_BSS buf_r[kBufferSamples];

// Engine state
static size_t write_idx = 0;
static size_t slice_len = 6000;
static int    win_slices = 8;   // slices in the loop at the current slice length
static int    play_step = 0;
static size_t play_off  = 0;
static int    order[kMaxWindowSlices];
static int    drift     = 0;
static int    dir       = 1;

static float  scramble = 0.0f;
static bool   frozen   = false;
static int    mode     = MODE_SCRAMBLE;
static bool   bypass   = false;
static float  flash    = 0.0f;
static float  wflash   = 0.0f;
static uint32_t rng_state = 0x9E3779B9u;

// Tap tempo
static float    tempo_ms         = kDefaultBeatMs;
static uint32_t last_tap_ms      = 0;
static float    tap_intervals[4] = {0.0f, 0.0f, 0.0f, 0.0f};
static int      tap_count        = 0;
static bool     first_tap        = true;

// std::clamp is C++17, this builds as gnu++14
static float ClampF(float x, float lo, float hi)
{
    if(x < lo)
        return lo;
    if(x > hi)
        return hi;
    return x;
}

static size_t ClampI(size_t x, size_t lo, size_t hi)
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

// Start from the in-order window and apply `swaps` random transpositions.
// Zero swaps means you hear the phrase back exactly as played.
static void BuildOrder()
{
    for(int i = 0; i < win_slices; i++)
        order[i] = i;

    int swaps = (int)(scramble * kMaxSwaps + 0.5f);
    for(int s = 0; s < swaps; s++)
    {
        int a = (int)(RandF() * (float)win_slices);
        int b = (int)(RandF() * (float)win_slices);
        if(a >= win_slices)
            a = win_slices - 1;
        if(b >= win_slices)
            b = win_slices - 1;
        int t    = order[a];
        order[a] = order[b];
        order[b] = t;
    }
}

// Which source slice feeds the current output step
static int SourceStep(int step)
{
    int s;
    switch(mode)
    {
        case MODE_REVERSE: s = win_slices - 1 - step; break;
        case MODE_PINGPONG: s = dir > 0 ? step : win_slices - 1 - step; break;
        case MODE_DRIFT: s = (step + drift) % win_slices; break;
        default: s = step; break;
    }
    if(s < 0)
        s = 0;
    if(s >= win_slices)
        s = win_slices - 1;
    return order[s];
}

// Short fade at every slice edge so the jumps don't click
static float SliceEnv()
{
    float env = 1.0f;
    if(play_off < kFadeSamples)
        env = (float)(play_off + 1) / (float)kFadeSamples;

    size_t remaining = slice_len - play_off;
    if(remaining <= kFadeSamples)
    {
        float out_fade = (float)remaining / (float)kFadeSamples;
        if(out_fade < env)
            env = out_fade;
    }
    return env;
}

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t                                size)
{
    float inl, inr;

    pod.ProcessAnalogControls();
    pod.ProcessDigitalControls();

    // Knob 1 -> division of the beat, Knob 2 -> scramble amount
    float div_knob = pod.knob1.Process();
    scramble       = pod.knob2.Process();

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

    // Button 2 -> press for a fresh shuffle, hold to freeze the current loop
    if(pod.button2.RisingEdge())
        BuildOrder();

    frozen = pod.button2.Pressed();

    // Encoder turn -> mode, encoder press -> bypass
    int32_t inc = pod.encoder.Increment();
    if(inc != 0)
        mode = ((mode + inc) % MODE_LAST + MODE_LAST) % MODE_LAST;

    if(pod.encoder.RisingEdge())
        bypass = !bypass;

    // Slice length follows the tempo immediately, so taps are audible at once
    int div_idx = (int)(div_knob * (float)(kNumDivisions - 1) + 0.5f);
    if(div_idx < 0)
        div_idx = 0;
    if(div_idx >= kNumDivisions)
        div_idx = kNumDivisions - 1;

    float slice_ms = tempo_ms / kDivisions[div_idx];
    slice_len      = ClampI((size_t)(slice_ms * 0.001f * pod.AudioSampleRate()),
                            kMinSliceSamples,
                            kMaxSliceSamples);

    // Long slices get fewer of them, so the loop stays around two seconds and
    // the latency never runs away from you
    int new_win = (int)(kMaxWindowSamples / slice_len);
    if(new_win < kMinWindowSlices)
        new_win = kMinWindowSlices;
    if(new_win > kMaxWindowSlices)
        new_win = kMaxWindowSlices;

    if(new_win != win_slices)
    {
        win_slices = new_win;
        play_step  = 0;
        play_off   = 0;
        drift      = 0;
        BuildOrder();
    }

    const size_t window = (size_t)win_slices * slice_len;

    for(size_t i = 0; i < size; i += 2)
    {
        inl = in[i];
        inr = in[i + 1];

        if(bypass)
        {
            out[i]     = inl;
            out[i + 1] = inr;
            continue;
        }

        // Freezing stops the write head, which parks the window in place
        if(!frozen)
        {
            buf_l[write_idx] = inl;
            buf_r[write_idx] = inr;
            if(++write_idx >= kBufferSamples)
                write_idx = 0;
        }

        // A shorter slice can leave the play head past the end of its slice
        if(play_off >= slice_len)
        {
            play_off = 0;
            if(++play_step >= win_slices)
                play_step = 0;
        }

        // Read head sits one window behind the write head
        size_t src_off = (size_t)SourceStep(play_step) * slice_len + play_off;
        size_t src = write_idx + kBufferSamples - window + src_off;
        if(src >= kBufferSamples)
            src -= kBufferSamples;
        if(src >= kBufferSamples)
            src -= kBufferSamples;

        float env = SliceEnv();
        out[i]     = buf_l[src] * env;
        out[i + 1] = buf_r[src] * env;

        play_off++;
        if(play_off >= slice_len)
        {
            play_off = 0;
            flash    = 1.0f;

            if(++play_step >= win_slices)
            {
                play_step = 0;
                BuildOrder();
                drift = (drift + 1) % win_slices;
                dir   = -dir;
                wflash = 1.0f;
            }
        }

        flash  -= flash * kFlashDecay;
        wflash -= wflash * kWindowFlashDecay;
    }

    // LED 1 flashes per slice, LED 2 shows the mode and pulses per window
    if(bypass)
    {
        pod.led1.Set(0.0f, 0.0f, 0.0f);
        pod.led2.Set(0.0f, 0.0f, 0.0f);
    }
    else
    {
        float f = flash;
        pod.led1.Set(f * 0.9f, f * 0.9f, f);

        float b = (0.35f + 0.65f * wflash) * (frozen ? 1.0f : 0.8f);
        switch(mode)
        {
            case MODE_SCRAMBLE: pod.led2.Set(0.0f, b * 0.8f, b); break;
            case MODE_REVERSE: pod.led2.Set(b * 0.2f, b, b * 0.3f); break;
            case MODE_PINGPONG: pod.led2.Set(b, b * 0.25f, 0.0f); break;
            case MODE_DRIFT: pod.led2.Set(b * 0.6f, b * 0.2f, b); break;
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
        buf_l[i] = buf_r[i] = 0.0f;

    rng_state = 0x9E3779B9u ^ System::GetNow();
    BuildOrder();

    slice_len = (size_t)(0.125f * sample_rate);

    pod.StartAdc();
    pod.StartAudio(AudioCallback);

    while(1) {}
}
