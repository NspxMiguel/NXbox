// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cmath>

namespace EdenXbox::Ui {

using Clock = std::chrono::steady_clock;

// The motion scale of docs/nxbox-ui.md: four durations and one curve for everything.
inline constexpr std::chrono::milliseconds kDurationQuick{150};
inline constexpr std::chrono::milliseconds kDurationItem{220};
inline constexpr std::chrono::milliseconds kDurationPanel{320};
inline constexpr std::chrono::milliseconds kDurationScreen{400};

// A CSS-style cubic-bezier(x1, y1, x2, y2) easing curve.
class CubicBezier {
public:
    // The two control points (p1x, p1y) and (p2x, p2y). Not named x1/y1: <math.h> declares a
    // function y1(), which a parameter of that name would hide.
    constexpr CubicBezier(float p1x, float p1y, float p2x, float p2y)
        : p1x_(p1x), p1y_(p1y), p2x_(p2x), p2y_(p2y) {}

    // Maps the elapsed fraction of the duration (0..1) to the eased progress (0..1).
    float operator()(float t) const {
        if (t <= 0.0f) {
            return 0.0f;
        }
        if (t >= 1.0f) {
            return 1.0f;
        }
        // Bisection on the curve parameter: x(s) is monotonic for the curves in use.
        float lower = 0.0f;
        float upper = 1.0f;
        float s = t;
        for (int i = 0; i < 24; ++i) {
            const float x = Coordinate(p1x_, p2x_, s);
            if (std::fabs(x - t) < 1e-5f) {
                break;
            }
            if (x < t) {
                lower = s;
            } else {
                upper = s;
            }
            s = (lower + upper) * 0.5f;
        }
        return Coordinate(p1y_, p2y_, s);
    }

private:
    // One coordinate of the bezier at parameter s, with the end points fixed at 0 and 1.
    static float Coordinate(float c1, float c2, float s) {
        const float inverse = 1.0f - s;
        return 3.0f * inverse * inverse * s * c1 + 3.0f * inverse * s * s * c2 + s * s * s;
    }

    float p1x_;
    float p1y_;
    float p2x_;
    float p2y_;
};

inline constexpr CubicBezier kEase{0.2f, 0.8f, 0.2f, 1.0f};

// A value that eases from wherever it is to a new target.
class Tween {
public:
    explicit Tween(float value = 0.0f) : from_(value), to_(value) {}

    // Starts moving to `target` from the current value, so retargeting mid-flight does not jump.
    void To(float target, Clock::time_point now, Clock::duration span = kDurationItem) {
        from_ = Value(now);
        to_ = target;
        start_ = now;
        span_ = span;
    }

    float Value(Clock::time_point now) const {
        if (span_.count() <= 0 || now >= start_ + span_) {
            return to_;
        }
        if (now <= start_) {
            return from_;
        }
        const float elapsed = std::chrono::duration<float>(now - start_).count() /
                              std::chrono::duration<float>(span_).count();
        return from_ + (to_ - from_) * kEase(elapsed);
    }

    float Target() const {
        return to_;
    }

private:
    float from_;
    float to_;
    Clock::time_point start_{};
    Clock::duration span_{};
};

} // namespace EdenXbox::Ui
