#include "pipeline/pipeline_node.h"
#include <stdexcept>
#include <algorithm>

namespace lexus2k::pipeline
{
    bool INode::_start() noexcept
    {
        for (auto it = m_pads.begin(); it != m_pads.end(); ++it)
        {
            if (!it->second->start()) {
                for (;it != m_pads.begin();) {
                    it--;
                    it->second->stop();
                }
                return false;
            }
        }
        if (!start()) {
            _stop();
            return false;
        }
        return true;
    }

    void INode::_stop() noexcept
    {
        stop();
        for (auto it = m_pads.begin(); it != m_pads.end(); ++it)
        {
            it->second->stop();
        }
    }

    bool INode::pushPacket(const std::string &name, std::shared_ptr<IPacket> packet, uint32_t timeout) const noexcept
    {
        auto* pad = getPadByName(name, PadType::INPUT);
        if (!pad)
        {
            return false; // Pad not found
        }
        return pad->pushPacket(packet, timeout);
    }

    IPad& INode::operator[](const std::string &name) const
    {
        auto* pad = getPadByName(name);
        if (!pad)
        {
            throw std::runtime_error("Pad not found");
        }
        return *pad;
    }

    IPad& INode::operator[](size_t index) const
    {
        auto* pad = getPadByIndex(index);
        if (!pad)
        {
            throw std::runtime_error("Pad not found");
        }
        return *pad;
    }

    // Helper method to find a pad by name
    IPad* INode::getPadByName(const std::string& name, PadType type) const noexcept
    {
        auto it = std::find_if(m_pads.begin(), m_pads.end(),
            [&name, &type](const auto& pair) {
                return pair.first == name && (type == PadType::UNDEFINED || pair.second->getType() == type);
            });
        return it != m_pads.end() ? it->second.get() : nullptr;
    }

    IPad* INode::getPadByIndex(size_t index) const noexcept
    {
        if (index < m_pads.size())
        {
            return m_pads[index].second.get();
        }
        return nullptr;
    }
}