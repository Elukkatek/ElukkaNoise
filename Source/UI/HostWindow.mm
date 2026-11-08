#import <Cocoa/Cocoa.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "HostWindow.h"

bool keepHostWindowVisible (juce::Component& editor)
{
    auto* peer = editor.getPeer();
    if (peer == nullptr) return false;
    auto* view = (NSView*) peer->getNativeHandle();
    NSWindow* window = view != nil ? [view window] : nil;
    if (window == nil) return false;
    [window setHidesOnDeactivate: NO];
    return true;
}
