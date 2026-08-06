// IUcpTransmission.h
#pragma once

#include <ppp/transmissions/ITransmission.h>
#include <ucp/ucp_connection.h>
#include <ucp/ucp_datagram_network.h>

namespace ppp {
    namespace transmissions {
        /**
         * @brief UCP transmission adapter.
         *
         * Bridges the UCP (Universal Communication Protocol) callback-based
         * connection API onto openppp2's coroutine-based ITransmission
         * interface.  UCP runs over UDP and provides KCC 2.0 geodesic
         * congestion control, GF(256) Reed-Solomon FEC, five-path recovery
         * and dynamic CID rotation -- the wire data itself is plaintext and
         * is encrypted by openppp2's existing protocol/transport ciphers
         * (HandshakeClient/HandshakeServer + Encrypt/Decrypt).
         *
         * Two construction modes are provided:
         *  - Client mode:  owns a UcpDatagramNetwork and the UcpConnection
         *                  created by it (network->CreateConnection).  A
         *                  dedicated thread drives network->DoEvents().
         *  - Server mode:  receives a raw UcpConnection* accepted by a
         *                  UcpServer (the server retains ownership).  The
         *                  hosting VirtualEthernetSwitcher drives DoEvents().
         */
        class IUcpTransmission : public ITransmission {
            friend class                                                                        ITransmissionQoS;

        public:
            typedef std::shared_ptr<ucp::UcpDatagramNetwork>                                    UcpNetworkPtr;
            typedef std::shared_ptr<ucp::UcpConnection>                                         UcpConnectionPtr;
            typedef std::pair<std::shared_ptr<Byte>, int>                                       RxChunk;
            typedef ppp::list<RxChunk>                                                          RxQueue;

        public:
            /**
             * @brief Client-mode constructor: takes ownership of the network
             *        and the connection produced by it.
             */
            IUcpTransmission(
                const ContextPtr&                                                               context,
                const StrandPtr&                                                                strand,
                const AppConfigurationPtr&                                                      configuration,
                const UcpNetworkPtr&                                                            network,
                const UcpConnectionPtr&                                                         connection,
                const boost::asio::ip::tcp::endpoint&                                           remoteEP) noexcept;
            /**
             * @brief Server-mode constructor: the caller (UcpServer) keeps
             *        ownership of the connection; only the raw pointer is stored.
             */
            IUcpTransmission(
                const ContextPtr&                                                               context,
                const StrandPtr&                                                                strand,
                const AppConfigurationPtr&                                                      configuration,
                ucp::UcpConnection*                                                             connection) noexcept;
            virtual ~IUcpTransmission()                                                                     noexcept;

        public:
            virtual void                                                                        Dispose() noexcept override;
            virtual boost::asio::ip::tcp::endpoint                                              GetRemoteEndPoint() noexcept override;
            virtual std::shared_ptr<Byte>                                                       ReadBytes(YieldContext& y, int length) noexcept;
            /** @brief Starts the UCP receive pump; call once after construction. */
            bool                                                                                StartReceive() noexcept;
            /** @brief True once Dispose/Finalize has run; the connection pointer
             *         returned by GetUcpConnection() is NULLPTR afterwards. */
            bool                                                                                IsDisposed() const noexcept { return disposed_; }
            /** @brief Returns the raw UCP connection pointer, or NULLPTR once
             *         the transmission has been finalized. */
            ucp::UcpConnection*                                                                 GetUcpConnection() const noexcept { return connection_; }

        protected:
            virtual std::shared_ptr<Byte>                                                       DoReadBytes(YieldContext& y, int length) noexcept;
            virtual bool                                                                        DoWriteBytes(std::shared_ptr<Byte> packet, int offset, int packet_length, const AsynchronousWriteBytesCallback& cb) noexcept;

        private:
            void                                                                                Finalize() noexcept;
            virtual bool                                                                        ShiftToScheduler() noexcept override;
            bool                                                                                BeginRead(YieldContext& y) noexcept;
            void                                                                                WakeupReaders() noexcept;
            void                                                                                WakeupAllReaders() noexcept;
            void                                                                                OnConnectionClosed() noexcept;
            void                                                                                StartDriving() noexcept;
            void                                                                                DriveLoop() noexcept;
            void                                                                                ReceiveMore(const std::shared_ptr<Byte>& buffer) noexcept;

        private:
            bool                                                                                disposed_ = false;
            bool                                                                                closed_ = false;
            bool                                                                                driving_ = false;
            bool                                                                                on_disconnected_registered_ = false;
            std::thread                                                                         driving_thread_;
            std::mutex                                                                          syncobj_;
            ucp::UcpConnection*                                                                 connection_ = NULLPTR;   ///< raw pointer; owned by UcpServer in server mode.
            UcpConnectionPtr                                                                    connection_owner_;       ///< client-mode ownership holder.
            UcpNetworkPtr                                                                       network_;                ///< client-mode ownership holder.
            boost::asio::ip::tcp::endpoint                                                      remoteEP_;
            RxQueue                                                                             rx_queue_;
            int                                                                                 rx_offset_ = 0;          ///< offset inside the front chunk.
            int                                                                                 rx_count_ = 0;           ///< total buffered bytes.
            ppp::list<std::pair<std::shared_ptr<std::atomic<int>>, YieldContext*>>              readers_;
        };
    }
}
