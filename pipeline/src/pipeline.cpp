#include "pipeline/pipeline.h"

namespace lexus2k::pipeline
{
    Pipeline::~Pipeline()
    {
        stop();
    }

    bool Pipeline::start() noexcept
    {
        for (auto node = m_nodes.begin(); node != m_nodes.end(); node++)
        {
            if (!node->get()->_start()) {
                for (;node != m_nodes.begin();) {
                    node--;
                    node->get()->_stop();
                }
                return false;
            }
        }
        return true;
    }

    void Pipeline::stop() noexcept
    {
        for (auto& node: m_nodes)
        {
            node->_stop();
        }
    }
}