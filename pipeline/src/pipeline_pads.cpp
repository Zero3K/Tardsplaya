#include "pipeline/pipeline_pads.h"
#include <iostream>

namespace lexus2k::pipeline
{
    /// @brief SimplePad pad

    bool SimplePad::queuePacket(std::shared_ptr<IPacket> packet, uint32_t timeout) noexcept
    {
        return processPacket(packet, timeout);
    }

    /// @brief Queued pad

    bool QueuePad::queuePacket(std::shared_ptr<IPacket> packet, uint32_t timeout) noexcept
    {
        std::unique_lock<std::mutex> lock(m_mutex);

        // Wait for space in the queue or timeout
        bool hasSpace = m_hasSpace.wait_for(lock, std::chrono::milliseconds(timeout), 
            [this] { return !m_isRunning.load(std::memory_order_relaxed) || m_queue.size() < m_maxQueueSize; });

        if (!hasSpace || !m_isRunning.load(std::memory_order_relaxed))
        {
            return false; // Timeout or pad is not running
        }

        // Add the packet to the queue
        m_queue.emplace_back(timeout, packet);
        lock.unlock();
        m_hasPackets.notify_one(); // Notify waiting threads
        return true;
    }

    bool QueuePad::start() noexcept
    {
        if (m_isRunning.load(std::memory_order_relaxed) || m_thread.joinable())
        {
            return true; // Already running
        }

        m_isRunning.store(true, std::memory_order_relaxed);

        // Start the processing thread
        m_thread = std::thread([this]() {
            while (m_isRunning.load(std::memory_order_relaxed))
            {
                std::pair<uint32_t,std::shared_ptr<IPacket>> packet;

                {
                    std::unique_lock<std::mutex> lock(m_mutex);

                    // Wait for packets or stop signal
                    m_hasPackets.wait(lock, [this] { return !m_isRunning || !m_queue.empty(); });

                    if (!m_isRunning.load(std::memory_order_relaxed) && m_queue.empty())
                    {
                        break; // Exit if stopped and no packets remain
                    }

                    // Retrieve the next packet
                    packet = std::move(m_queue.front());
                    m_queue.erase(m_queue.begin());
                }

                // Notify space availability
                m_hasSpace.notify_one();

                // Process the packet
                processPacket(packet.second, packet.first);
            }
        });
        return true;
    }

    void QueuePad::stop() noexcept
    {
        if (!m_thread.joinable())
        {
            return; // Not running
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_isRunning.store(false, std::memory_order_relaxed);
        }

        // Notify all waiting threads
        m_hasPackets.notify_all();
        m_hasSpace.notify_all();

        // Join the thread
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }
}