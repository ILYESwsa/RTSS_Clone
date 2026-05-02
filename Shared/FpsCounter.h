#pragma once
// ============================================================
// FpsCounter.h — Accurate FPS measurement via QPC
// Called from inside the hooked Present/EndScene functions.
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

class FpsCounter
{
public:
    FpsCounter()
        : m_fps(0.f), m_frameTimeMs(0.f), m_totalFrames(0)
        , m_windowFrames(0), m_freqInv(0.0)
    {
        LARGE_INTEGER freq;
        ::QueryPerformanceFrequency(&freq);
        // Pre-compute inverse so divide is a multiply at runtime
        m_freqInv = 1.0 / static_cast<double>(freq.QuadPart);

        ::QueryPerformanceCounter(&m_lastFrame);
        m_windowStart = m_lastFrame;
    }

    // Call once per presented frame (inside Present hook).
    // Returns current FPS (rolling 1-second window).
    float OnFrame()
    {
        LARGE_INTEGER now;
        ::QueryPerformanceCounter(&now);

        // Frame time of this individual frame
        double dt = static_cast<double>(now.QuadPart - m_lastFrame.QuadPart) * m_freqInv;
        m_frameTimeMs = static_cast<float>(dt * 1000.0);
        m_lastFrame = now;

        ++m_totalFrames;
        ++m_windowFrames;

        // Accumulate over a 1-second sliding window
        double elapsed = static_cast<double>(now.QuadPart - m_windowStart.QuadPart) * m_freqInv;
        if (elapsed >= 1.0)
        {
            m_fps = static_cast<float>(static_cast<double>(m_windowFrames) / elapsed);
            m_windowFrames = 0;
            m_windowStart  = now;
        }

        return m_fps;
    }

    float    GetFps()         const { return m_fps; }
    float    GetFrameTimeMs() const { return m_frameTimeMs; }
    uint64_t GetTotalFrames() const { return m_totalFrames; }

private:
    float          m_fps;
    float          m_frameTimeMs;
    uint64_t       m_totalFrames;
    uint64_t       m_windowFrames;
    double         m_freqInv;
    LARGE_INTEGER  m_lastFrame;
    LARGE_INTEGER  m_windowStart;
};
