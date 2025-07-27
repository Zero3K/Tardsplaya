#ifndef PIPELINE_H
#define PIPELINE_H

#include <memory>
#include <vector>
#include <cstdint>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <string_view>
#include <type_traits>

#include "pipeline_packet.h"
#include "pipeline_pad.h"
#include "pipeline_pads.h"
#include "pipeline_node.h"
#include "pipeline_nodes.h"

namespace lexus2k::pipeline
{
    /**
     * @class Pipeline
     * @brief Manages a collection of nodes and their connections.
     */
    class Pipeline
    {
    public:
        /**
         * @brief Default constructor.
         */
        Pipeline() = default;

        /**
         * @brief Destructor.
         */
        ~Pipeline();

        /**
         * @brief Adds a new node to the pipeline.
         * @tparam T The type of the node to add.
         * @tparam Args The arguments to pass to the node's constructor.
         * @param args Arguments for the node's constructor.
         * @return A pointer to the newly added node.
         */
        template <typename T, typename... Args>
        T* addNode(Args&&... args)
        {
            auto node = std::make_shared<T>(std::forward<Args>(args)...);
            m_nodes.push_back(node);
            return node.get();
        }

        /**
         * @brief Adds a lambda-based node to the pipeline.
         * @tparam T The type of the lambda function.
         * @param lambda The lambda function to process packets.
         * @return A pointer to the newly added LambdaNode.
         */
        template <typename T, typename = std::enable_if_t<std::is_invocable_v<T>>>
        ILambdaNode<T>* addNode(T&& lambda)
        {
            auto node = std::make_shared<ILambdaNode<T>>(lambda);
            m_nodes.push_back(node);
            return node.get();
        }

        /**
         * @brief Connects two pads.
         * @param output The output pad.
         * @param input The input pad.
         */
        void connect(IPad& output, IPad& input) const noexcept
        {
            output.then(input);
        }

        /**
         * @brief Starts all nodes in the pipeline.
         */
        bool start() noexcept;

        /**
         * @brief Stops all nodes in the pipeline.
         */
        void stop() noexcept;

    private:
        std::vector<std::shared_ptr<INode>> m_nodes; ///< Collection of nodes in the pipeline.
    };

} // namespace lexus2k::pipeline

#endif // PIPELINE_H