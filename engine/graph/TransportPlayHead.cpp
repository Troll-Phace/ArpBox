#include "TransportPlayHead.h"

namespace arpbox::engine
{
// RT-SAFE:
juce::Optional<juce::AudioPlayHead::PositionInfo> TransportPlayHead::getPosition () const
{
    PositionInfo info;

    // Everything below comes from the transport's LATCHED block-start state, so a
    // plugin that queries the playhead mid-block still sees the block's start
    // position — which is what plugins expect and what makes their sample-offset
    // arithmetic against the incoming MidiBuffer correct.
    info.setBpm (transport.bpm ());
    info.setPpqPosition (transport.blockStartPpq ());
    info.setPpqPositionOfLastBarStart (transport.ppqOfLastBarStart ());
    info.setTimeInSamples (transport.blockStartTimeInSamples ());
    info.setTimeInSeconds (transport.blockStartTimeInSeconds ());
    info.setIsPlaying (transport.isPlaying ());

    // Fixed 4/4 for the MVP (Transport::quarterNotesPerBar); a real meter model
    // arrives with the post-MVP tempo/meter map.
    info.setTimeSignature (TimeSignature { 4, 4 });

    // ARPBOX is not a host with a loop region or a record-arm that plugins should
    // see: state these explicitly rather than leaving them unset, so a plugin that
    // reads them gets a definite answer. (The master-section WAV recorder in Phase
    // 18 captures audio downstream; it is not a timeline record pass.)
    info.setIsLooping (false);
    info.setIsRecording (false);

    return info;
}
} // namespace arpbox::engine
