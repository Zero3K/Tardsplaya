#ifndef LEXUS2K_PIPELINE_NODES_H
#define LEXUS2K_PIPELINE_NODES_H

#include <memory>
#include <string>
#include "pipeline_node.h"

namespace lexus2k::pipeline
{
    /**
     * @class ILambdaNode
     * @brief A node that processes packets using a lambda function.
     * @tparam Lambda The type of the lambda function.
     */
    template <typename Lambda>
    class ILambdaNode : public INode
    {
    public:
        /**
         * @brief Constructs a LambdaNode with the given lambda function.
         * @param lambda The lambda function to process packets.
         */
        explicit ILambdaNode(Lambda lambda) : m_func(lambda) {}

    protected:
        /**
         * @brief Processes a packet using the lambda function.
         * @param packet The packet to process.
         * @param inputPad The input pad that received the packet.
         */
        bool processPacket(std::shared_ptr<IPacket> packet, IPad& inputPad, uint32_t timeoutMs) noexcept override
        {
            return m_func(packet, inputPad);
        }

    private:
        Lambda m_func; ///< The lambda function for processing packets.
    };

    /**
     * @class ISplitter
     * @brief A node that forwards packets to multiple output pads.
     */
    class ISplitter : public INode
    {
    public:
        /**
         * @brief Default constructor.
         */
        explicit ISplitter() : INode() {};

        /**
         * @brief Default destructor.
         */
        virtual ~ISplitter() = default;

    protected:
        /**
         * @brief Processes a packet and forwards it to all output pads.
         * @param packet The packet to process.
         * @param inputPad The input pad that received the packet.
         * @param timeoutMs The timeout for the operation.
         * @return True if the packet was successfully processed, false otherwise.
         */
        bool processPacket(std::shared_ptr<IPacket> packet, IPad& inputPad, uint32_t timeoutMs) noexcept override;
    };

    template<size_t N, typename InputPad, typename... Args>
    class Splitter : public ISplitter
    {
    public:
        explicit Splitter() : ISplitter() {
            addInput<InputPad>("input", Args()...);
            for (size_t i = 1; i <= N; ++i) {
                addOutput("output_" + std::to_string(i));
            }
        }

        /**
         * @brief Default destructor.
         */
        ~Splitter() = default;
    };

} // namespace lexus2k::pipeline

#endif // LEXUS2K_PIPELINE_NODES_H