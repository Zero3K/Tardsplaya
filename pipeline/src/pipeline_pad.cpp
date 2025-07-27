#include "pipeline/pipeline_pad.h"
#include "pipeline/pipeline.h"

namespace lexus2k::pipeline
{
    bool IPad::pushPacket(std::shared_ptr<IPacket> packet, uint32_t timeout) noexcept
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_padType != PadType::INPUT)
        {
            auto linkedPad = m_linkedPad;
            lock.unlock();
            if (linkedPad != nullptr)
            {
                return linkedPad->pushPacket(packet, timeout);
            }
            return false; // No linked pad available
        }
        lock.unlock();
        return queuePacket(packet, timeout);
    }

    INode& IPad::then(IPad& pad) noexcept
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_padType == PadType::UNDEFINED) {
            m_padType = PadType::OUTPUT;
        }
        if (pad.m_padType == PadType::UNDEFINED) {
            pad.m_padType = PadType::INPUT;
        }
        m_linkedPad = &pad;
        return pad.node();
    }

    void IPad::then() noexcept
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_padType == PadType::UNDEFINED) {
            m_padType = PadType::OUTPUT;
        }
        m_linkedPad = nullptr;
    }

    bool IPad::processPacket(std::shared_ptr<IPacket> packet, uint32_t timeout) noexcept
    {
        return (node()).processPacket(packet, *this, timeout); // Pass reference instead of pointer
    }
}