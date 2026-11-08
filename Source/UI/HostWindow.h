#pragma once

namespace juce { class Component; }

// macOS: hosts (e.g. Ableton Live) put plugin editors in windows that hide whenever the host app loses focus.
// This turns that off for the window holding `editor`, so the synth stays on screen until it is closed.
// Returns true once the window was found (the editor may not be inside a host window yet).
bool keepHostWindowVisible (juce::Component& editor);
