// Analog conditioning: deadzones, response curves and sensitivities (all user-tunable, saved
// with the bindings).
#pragma once

namespace litholattice {

struct AxisValue {
  float x = 0.f, y = 0.f;
};

struct AnalogSettings {
  float stickDeadzone = 0.20f;     ///< radial, fraction of full deflection
  float triggerDeadzone = 0.10f;   ///< linear
  float stickCurve = 2.0f;         ///< response exponent after the deadzone (1 = linear)
  float mouseSensitivity = 0.0025f;  ///< look radians per mouse pixel
  float stickLookSpeed = 3.0f;     ///< look radians per second at full deflection
  bool invertY = false;            ///< look up/down inverted
  float pressThreshold = 0.5f;     ///< analog value at which a button action counts as held

  bool operator==(const AnalogSettings&) const = default;
};

/// Radial deadzone on a stick: below `deadzone` -> 0; above, the magnitude is rescaled to 0..1
/// and shaped by `curve`, keeping the direction (no "plus sign" snapping on diagonals).
AxisValue applyRadialDeadzone(AxisValue raw, float deadzone, float curve);
/// Linear deadzone + curve for a single axis or trigger (sign kept).
float applyDeadzone(float raw, float deadzone, float curve);

}  // namespace litholattice
