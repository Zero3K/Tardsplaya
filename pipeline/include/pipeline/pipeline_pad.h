#ifndef LEXUS2K_PIPELINE_PAD_H
#define LEXUS2K_PIPELINE_PAD_H

#include <memory>
#include <vector>
#include <cstdint>
#include <mutex>
#include <condition_variable>

#include "pipeline_packet.h"

namespace lexus2k::pipeline
{
    class INode;

    /**
     * @enum PadType
     * @brief Represents the type of a pad in the pipeline.
     *
     * Pads can be of three types:
     * - `INPUT`: Represents an input pad that receives packets.
     * - `OUTPUT`: Represents an output pad that sends packets.
     * - `UNDEFINED`: Represents a pad with an undefined type.
     */
    enum class PadType
    {
        INPUT,    ///< Input pad type.
        OUTPUT,   ///< Output pad type.
        UNDEFINED ///< Undefined pad type.
    };

    /**
     * @class IPad
     * @brief Represents a pad in the pipeline for connecting nodes.
     *
     * The `IPad` class serves as the base class for all pads in the pipeline.
     * Pads are responsible for transferring packets between nodes and managing
     * their connections. Derived classes can implement specific behaviors for
     * queuing and processing packets.
     */
    class IPad {
    public:
        /**
         * @brief Deleted copy constructor.
         *
         * Prevents copying of `IPad` instances to ensure unique ownership.
         */
        IPad(const IPad&) = delete;

        /**
         * @brief Default constructor.
         *
         * Initializes the pad with an undefined type.
         */
        IPad() : m_padType(PadType::UNDEFINED) {}

        /**
         * @brief Virtual destructor.
         *
         * Ensures proper cleanup of derived classes when deleted through
         * a pointer to `IPad`.
         */
        virtual ~IPad() = default;

        /**
         * @brief Pushes a packet to the pad.
         *
         * Attempts to push a packet to the pad. The behavior depends on the
         * implementation of the `specific` pad method in derived classes.
         *
         * @param packet The packet to push.
         * @param timeout The timeout for the operation, in milliseconds.
         * @return `true` if the packet was successfully pushed, `false` otherwise.
         */
        bool pushPacket(std::shared_ptr<IPacket> packet, uint32_t timeout) noexcept;

        /**
         * @brief Gets the parent node of the pad.
         * @return A pointer to the parent node.
         */
        inline INode& node() const noexcept { return *m_parentNode; }

        /**
         * @brief Gets the type of the pad.
         * @return The type of the pad (`INPUT`, `OUTPUT`, or `UNDEFINED`).
         */
        inline PadType getType() const noexcept { return m_padType; }

        /**
         * 
         */
        inline size_t getIndex() const noexcept { return m_padIndex; }

        /**
         * @brief Connects this pad to another pad.
         *
         * Establishes a connection between this pad and the specified pad.
         * The connection is unidirectional, meaning packets sent from this pad
         * will be forwarded to the connected pad.
         *
         * @param pad The pad to connect to.
         * @return A reference to the parent node of the connected pad.
         */
        INode& then(IPad& pad) noexcept;

        /**
         * @brief Disconnects this pad from the connected pad.
         * 
         * This method is used to disconnect the pad from its connected pad.
         * It is typically called when the pipeline is stopped or when the
         * connection is no longer needed.
         */
        void then() noexcept;

        /**
         * @brief Starts the pad.
         *
         * This method can be overridden by derived classes to implement
         * specific startup behavior for the pad.
         */
        virtual bool start() noexcept { return true; }

        /**
         * @brief Stops the pad.
         *
         * This method can be overridden by derived classes to implement
         * specific shutdown behavior for the pad.
         */
        virtual void stop() noexcept {}

    protected:
        /**
         * @brief Queues a packet for processing.
         *
         * This pure virtual method must be implemented by derived classes
         * to define how packets are queued for processing.
         *
         * @param packet The packet to queue.
         * @param timeout The timeout for the operation, in milliseconds.
         * @return `true` if the packet was successfully queued, `false` otherwise.
         */
        virtual bool queuePacket(std::shared_ptr<IPacket> packet, uint32_t timeout) noexcept = 0;

        /**
         * @brief Processes a packet.
         *
         * Processes a packet and forwards it to the connected pad, if any.
         *
         * @param packet The packet to process.
         * @param timeout The timeout for the operation, in milliseconds.
         * @return `true` if the packet was successfully processed, `false` otherwise.
         */
        bool processPacket(std::shared_ptr<IPacket> packet, uint32_t timeout) noexcept;

    private:
        std::mutex m_mutex; ///< Mutex for thread safety.
        INode* m_parentNode = nullptr; ///< Pointer to the parent node of the pad.
        IPad* m_linkedPad = nullptr;   ///< Pointer to the connected pad.
        PadType m_padType = PadType::INPUT; ///< The type of the pad.
        size_t m_padIndex = 0; ///< The index of the pad in the parent node.

        /**
         * @brief Sets the parent node of the pad.
         * @param parent A pointer to the parent node.
         */
        inline void setParent(INode* parent) noexcept { m_parentNode = parent; }

        /**
         * @brief Sets the index of the pad in the parent node.
         * @param index The index to set.
         */
        inline void setIndex(size_t index) noexcept { m_padIndex = index; }

        /**
         * @brief Sets the type of the pad.
         * @param type The type to set (`INPUT`, `OUTPUT`, or `UNDEFINED`).
         */
        inline void setType(PadType type) noexcept { m_padType = type; }

        friend class INode;
    };

} // namespace lexus2k::pipeline

#endif // LEXUS2K_PIPELINE_PAD_H