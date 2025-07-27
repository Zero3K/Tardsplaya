#ifndef LEXUS2K_PIPELINE_NODE_H
#define LEXUS2K_PIPELINE_NODE_H

#include <vector>
#include <memory>
#include <string>
#include <type_traits>

#include "pipeline_pad.h"
#include "pipeline_pads.h"
#include "pipeline_packet.h"

namespace lexus2k::pipeline
{
    class Pipeline;

    /**
     * @class INode
     * @brief Represents a node in the pipeline, managing pads and processing packets.
     */
    class INode
    {
    public:
        /**
         * @brief Default constructor.
         */
        INode() = default;

        /**
         * @brief Deleted copy constructor.
         */
        INode(const INode&) = delete;

        /**
         * @brief Deleted copy assignment operator.
         */
        INode& operator=(const INode&) = delete;

        /**
         * @brief Virtual destructor.
         */
        virtual ~INode() = default;

        /**
         * @brief Pushes a packet to a pad by name.
         * @param name The name of the pad.
         * @param packet The packet to push.
         * @param timeout The timeout for the operation.
         * @return True if the packet was successfully pushed, false otherwise.
         */
        bool pushPacket(const std::string& name, std::shared_ptr<IPacket> packet, uint32_t timeout = 0) const noexcept;

        /**
         * @brief Adds a new input pad to the node.
         * @tparam T The type of the pad.
         * @param name The name of the pad.
         * @param args Additional arguments for the pad's constructor.
         * @return A reference to the newly added pad.
         */
        template <typename T, typename... Args>
        T& addInput(const std::string& name, Args&&... args)
        {
            auto pad = std::make_shared<T>(std::forward<Args>(args)...);
            pad->setType(PadType::INPUT);
            pad->setParent(this);
            m_pads.emplace_back(name, pad);
            pad->setIndex(m_pads.size() - 1);
            return *pad;
        }

        /**
         * @brief Adds a new input pad to the node.
         * @param name The name of the pad.
         * @param args Additional arguments for the pad's constructor.
         * @return A reference to the newly added pad.
         */
        template<typename... Args>
        auto& addInput(const std::string &name, Args&&... args)
        {
            return addInput<SimplePad>(name, std::forward<Args>(args)...);
        }

        /**
         * @brief Adds a new output pad to the node.
         * @param name The name of the pad.
         * @return A reference to the newly added pad.
         */
        auto& addOutput(const std::string &name)
        {
            auto pad = std::make_shared<SimplePad>();
            pad->setType(PadType::OUTPUT);
            pad->setParent(this);
            m_pads.emplace_back(name, pad);
            pad->setIndex(m_pads.size() - 1);
            return *pad;
        }

        /**
         * @brief Retrieves a pad by name.
         * @param name The name of the pad.
         * @return A reference to the pad.
         * @throws std::runtime_error if the pad is not found.
         */
        IPad& operator[](const std::string &name) const;

        /**
         * @brief Retrieves a pad by index.
         * @param index The index of the pad.
         * @return A reference to the pad.
         * @throws std::runtime_error if the pad is not found.
         */
        IPad& operator[](size_t index) const;

        /**
         * @brief Starts the node.
         */
        virtual bool start() noexcept { return true; }

        /**
         * @brief Stops the node.
         */
        virtual void stop() noexcept {};

    protected:
        /**
         * @brief Processes a packet received on an input pad.
         * @param packet The packet to process.
         * @param inputPad The input pad that received the packet.
         * @return True if the packet was successfully processed, false otherwise.
         */
        virtual bool processPacket(std::shared_ptr<IPacket> packet, IPad& inputPad, uint32_t timeoutMs) noexcept { return false; }

        // Helper method to find a pad by name
        IPad* getPadByName(const std::string& name, PadType type = PadType::UNDEFINED) const noexcept;

        IPad* getPadByIndex(size_t index) const noexcept;

        /**
         * @brief Starts the node. It guarantees that if any pad start fails,
         *        node will not be started
         * 
         */
        bool _start() noexcept;

        /**
         * @brief Stops the node.
         */
        void _stop() noexcept;

    private:
        std::vector<std::pair<std::string, std::shared_ptr<IPad>>> m_pads; ///< Collection of pads.

        friend class IPad;
        friend class Pipeline;
    };

    /**
     * @class Node
     * @brief A template class for processing packets derived from `IPacket`.
     */
    template <typename T, typename = std::enable_if_t<std::is_base_of_v<IPacket, T>>>
    class Node : public INode
    {
    public:
        Node() : INode() {}
        ~Node() = default;

    protected:
        bool processPacket(std::shared_ptr<IPacket> packet, IPad& inputPad, uint32_t timeoutMs) noexcept override final
        {
            auto derivedPacket = std::dynamic_pointer_cast<T>(packet);
            if (derivedPacket)
            {
                return processPacket(derivedPacket, inputPad, timeoutMs);
            }
            return false; // Packet type mismatch
        }

        virtual bool processPacket(std::shared_ptr<T> packet, IPad& inputPad, uint32_t timeoutMs) noexcept = 0;
    };

    /**
     * @class Node2
     * @brief A template class for processing packets of two types derived from `IPacket`.
     */
    template <typename T1, typename T2, typename = std::enable_if_t<std::is_base_of_v<IPacket, T1> && std::is_base_of_v<IPacket, T2>>>
    class Node2 : public INode
    {
    public:
        Node2() : INode() {}
        ~Node2() = default;

    protected:
        bool processPacket(std::shared_ptr<IPacket> packet, IPad& inputPad, uint32_t timeoutMs) noexcept override final
        {
            if (inputPad.getIndex() == 0) {
                auto derivedPacket = std::dynamic_pointer_cast<T1>(packet);
                if (derivedPacket)
                {
                    return processPacket(derivedPacket, inputPad, timeoutMs);
                }
            }
            else if (inputPad.getIndex() == 1)
            {
                auto derivedPacket = std::dynamic_pointer_cast<T2>(packet);
                if (derivedPacket)
                {
                    return processPacket(derivedPacket, inputPad, timeoutMs);
                }
            }
            return false; // Packet type mismatch
        }

        virtual bool processPacket(std::shared_ptr<T1> packet, IPad& inputPad, uint32_t timeoutMs) noexcept = 0;
        virtual bool processPacket(std::shared_ptr<T2> packet, IPad& inputPad, uint32_t timeoutMs) noexcept = 0;
    };
} // namespace lexus2k::pipeline

#endif // LEXUS2K_PIPELINE_NODE_H