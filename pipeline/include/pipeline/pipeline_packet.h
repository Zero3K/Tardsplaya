#ifndef PIPELINE_PACKET_H
#define PIPELINE_PACKET_H

namespace lexus2k::pipeline
{
    /**
     * @class IPacket
     * @brief Represents a generic data packet in the pipeline.
     *
     * The `IPacket` interface serves as the base class for all data packets
     * that are passed between nodes in the pipeline. Derived classes can
     * implement specific types of packets with additional data and functionality.
     */
    class IPacket {
    public:
        /**
         * @brief Virtual destructor.
         *
         * Ensures proper cleanup of derived classes when deleted through
         * a pointer to `IPacket`.
         */
        virtual ~IPacket() = default;

        virtual size_t serializeTo(void *ptr, size_t max_size) noexcept { return -1; }

        virtual size_t deserializeFrom(const void *ptr, size_t size) noexcept { return -1; }
    };

} // namespace lexus2k::pipeline

#endif // PIPELINE_PACKET_H