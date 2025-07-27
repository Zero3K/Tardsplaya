#ifndef PIPELINE_PADS_H
#define PIPELINE_PADS_H

#include "pipeline_pad.h"
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>

namespace lexus2k::pipeline
{
    /**
     * @class SimplePad
     * @brief A simple pad that immediately pushes packets to the next pad.
     *
     * The `SimplePad` class is a lightweight implementation of `IPad` that
     * forwards packets to the next connected pad without any queuing or
     * additional processing. It is suitable for scenarios where packets
     * need to be processed in real-time without delays.
     */
    class SimplePad : public IPad {
    public:
        /**
         * @brief Default constructor.
         *
         * Initializes the `SimplePad` instance.
         */
        SimplePad() : IPad() {}

        /**
         * @brief Default destructor.
         *
         * Ensures proper cleanup of the `SimplePad` instance.
         */
        ~SimplePad() = default;

    protected:
        /**
         * @brief Queues a packet for immediate forwarding.
         *
         * This method overrides the `queuePacket` method in the `IPad` base
         * class. It immediately forwards the packet to the next connected pad.
         *
         * @param packet The packet to queue.
         * @param timeout The timeout for the operation, in milliseconds.
         * @return `true` if the packet was successfully forwarded, `false` otherwise.
         */
        bool queuePacket(std::shared_ptr<IPacket> packet, uint32_t timeout) noexcept override;
    };

    /**
     * @class QueuePad
     * @brief A pad that queues packets for processing.
     *
     * The `QueuePad` class is an implementation of `IPad` that maintains a
     * queue of packets. Packets are processed in the order they are received,
     * and the queue size is configurable. This pad is suitable for scenarios
     * where packets need to be buffered before processing.
     */
    class QueuePad : public IPad
    {
    public:
        /**
         * @brief Template constructor with a compile-time queue size.
         *
         * Initializes the `QueuePad` instance with a fixed maximum queue size.
         *
         * @tparam N The maximum size of the queue.
         */
        template <size_t N>
        QueuePad() : IPad(), m_maxQueueSize(N) {}

        /**
         * @brief Constructor with a runtime-configurable queue size.
         *
         * Initializes the `QueuePad` instance with the specified maximum queue size.
         *
         * @param N The maximum size of the queue. Defaults to `4`.
         */
        QueuePad(size_t N = 4) : IPad(), m_maxQueueSize(N) {}

        /**
         * @brief Default destructor.
         *
         * Ensures proper cleanup of the `QueuePad` instance.
         */
        ~QueuePad() = default;

        /**
         * @brief Starts the queue processing thread.
         *
         * This method overrides the `start` method in the `IPad` base class.
         * It starts a background thread that processes packets from the queue.
         */
        bool start() noexcept override;

        /**
         * @brief Stops the queue processing thread.
         *
         * This method overrides the `stop` method in the `IPad` base class.
         * It stops the background thread and ensures all resources are cleaned up.
         */
        void stop() noexcept override;

    protected:
        /**
         * @brief Queues a packet for processing.
         *
         * This method overrides the `queuePacket` method in the `IPad` base
         * class. It adds the packet to the queue and notifies the processing
         * thread.
         *
         * @param packet The packet to queue.
         * @param timeout The timeout for the operation, in milliseconds.
         * @return `true` if the packet was successfully queued, `false` otherwise.
         */
        bool queuePacket(std::shared_ptr<IPacket> packet, uint32_t timeout) noexcept override;

    private:
        size_t m_maxQueueSize; ///< The maximum size of the queue.
        std::mutex m_mutex; ///< Mutex for synchronizing access to the queue.
        std::condition_variable m_hasPackets; ///< Condition variable for waiting on packets.
        std::condition_variable m_hasSpace; ///< Condition variable for waiting on available space.
        std::vector<std::pair<uint32_t, std::shared_ptr<IPacket>>> m_queue; ///< The packet queue.
        std::atomic_bool m_isRunning{false}; ///< Indicates whether the queue processing thread is running.
        std::thread m_thread; ///< The background thread for processing packets.
    };

} // namespace lexus2k::pipeline

#endif // PIPELINE_PADS_H