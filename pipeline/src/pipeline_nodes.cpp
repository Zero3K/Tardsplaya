#include "pipeline/pipeline_nodes.h"
#include <iostream>

namespace lexus2k::pipeline
{
    bool ISplitter::processPacket(std::shared_ptr<IPacket> packet, IPad& inputPad, uint32_t timeoutMs) noexcept
    {
        bool result = true;
        // Process the packet and send it to all output pads
        for(size_t index = 0;; index++) {
            auto pad = getPadByIndex(index);
            if (pad == nullptr) {
                break; // No more pads
            }
            if (pad->getType() == PadType::OUTPUT) {
                result = pad->pushPacket(packet, timeoutMs) && result;
            }
        }
        return result;
    }
}