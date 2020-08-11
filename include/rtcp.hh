#pragma once

#include <bitset>
#include <map>
#include <thread>
#include <vector>

#include "clock.hh"
#include "frame.hh"
#include "runner.hh"
#include "socket.hh"
#include "util.hh"

namespace uvg_rtp {

    class connection;

    enum ROLE {
        RECEIVER,
        SENDER
    };

    /* TODO: explain these constants */
    const int RTP_SEQ_MOD    = 1 << 16;
    const int MIN_SEQUENTIAL = 2;
    const int MAX_DROPOUT    = 3000;
    const int MAX_MISORDER   = 100;
    const int MIN_TIMEOUT    = 5000;

    struct rtcp_statistics {
        /* receiver stats */
        uint32_t received_pkts;  /* Number of packets received */
        uint32_t dropped_pkts;   /* Number of dropped RTP packets */
        uint32_t received_bytes; /* Number of bytes received excluding RTP Header */

        /* sender stats */
        uint32_t sent_pkts;   /* Number of sent RTP packets */
        uint32_t sent_bytes;  /* Number of sent bytes excluding RTP Header */

        uint32_t jitter;      /* TODO: */
        uint32_t transit;     /* TODO: */

        /* Receiver clock related stuff */
        uint64_t initial_ntp; /* Wallclock reading when the first RTP packet was received */
        uint32_t initial_rtp; /* RTP timestamp of the first RTP packet received */
        uint32_t clock_rate;  /* Rate of the clock (used for jitter calculations) */

        uint32_t lsr;                     /* Middle 32 bits of the 64-bit NTP timestamp of previous SR */
        uvg_rtp::clock::hrc::hrc_t sr_ts; /* When the last SR was received (used to calculate delay) */

        uint16_t max_seq;  /* Highest sequence number received */
        uint16_t base_seq; /* First sequence number received */
        uint16_t bad_seq;  /* TODO:  */
        uint16_t cycles;   /* Number of sequence cycles */
    };

    struct rtcp_participant {
        uvg_rtp::socket *socket; /* socket associated with this participant */
        sockaddr_in address;     /* address of the participant */
        struct rtcp_statistics stats; /* RTCP session statistics of the participant */

        int probation;           /* has the participant been fully accepted to the session */
        int role;                /* is the participant a sender or a receiver */

        /* Save the latest RTCP packets received from this participant
         * Users can query these packets using the SSRC of participant */
        uvg_rtp::frame::rtcp_sender_frame   *s_frame;
        uvg_rtp::frame::rtcp_receiver_frame *r_frame;
        uvg_rtp::frame::rtcp_sdes_frame     *sdes_frame;
        uvg_rtp::frame::rtcp_app_frame      *app_frame;
    };

    class rtcp : public runner {
        public:
            rtcp(uint32_t ssrc, bool receiver);
            rtcp(uvg_rtp::rtp *rtp);
            ~rtcp();

            /* start the RTCP runner thread
             *
             * return RTP_OK on success and RTP_MEMORY_ERROR if the allocation fails */
            rtp_error_t start();

            /* End the RTCP session and send RTCP BYE to all participants
             *
             * return RTP_OK on success */
            rtp_error_t stop();

            /* Generate either RTCP Sender or Receiver report and sent it to all participants
             * Return RTP_OK on success and RTP_ERROR on error */
            rtp_error_t generate_report();

            /* Handle different kinds of incoming packets
             *
             * These routines will convert the fields of "frame" from network to host byte order
             *
             * Currently nothing's done with valid packets, at some point an API for
             * querying these reports is implemented
             *
             * Return RTP_OK on success and RTP_ERROR on error */
            rtp_error_t handle_sender_report_packet(uvg_rtp::frame::rtcp_sender_frame *frame, size_t size);
            rtp_error_t handle_receiver_report_packet(uvg_rtp::frame::rtcp_receiver_frame *frame, size_t size);
            rtp_error_t handle_sdes_packet(uvg_rtp::frame::rtcp_sdes_frame *frame, size_t size);
            rtp_error_t handle_bye_packet(uvg_rtp::frame::rtcp_bye_frame *frame, size_t size);
            rtp_error_t handle_app_packet(uvg_rtp::frame::rtcp_app_frame *frame, size_t size);

            /* Handle incoming RTCP packet (first make sure it's a valid RTCP packet)
             * This function will call one of the above functions internally
             *
             * Return RTP_OK on success and RTP_ERROR on error */
            rtp_error_t handle_incoming_packet(uint8_t *buffer, size_t size);

            /* Send "frame" to all participants
             *
             * These routines will convert all necessary fields to network byte order
             *
             * Return RTP_OK on success
             * Return RTP_INVALID_VALUE if "frame" is in some way invalid
             * Return RTP_SEND_ERROR if sending "frame" did not succeed (see socket.hh for details) */
            rtp_error_t send_sender_report_packet(uvg_rtp::frame::rtcp_sender_frame *frame);
            rtp_error_t send_receiver_report_packet(uvg_rtp::frame::rtcp_receiver_frame *frame);
            rtp_error_t send_sdes_packet(uvg_rtp::frame::rtcp_sdes_frame *frame);
            rtp_error_t send_bye_packet(uvg_rtp::frame::rtcp_bye_frame *frame);
            rtp_error_t send_app_packet(uvg_rtp::frame::rtcp_app_frame *frame);

            /* Return the latest RTCP packet received from participant of "ssrc"
             * Return nullptr if we haven't received this kind of packet or if "ssrc" doesn't exist
             *
             * NOTE: Caller is responsible for deallocating the memory */
            uvg_rtp::frame::rtcp_sender_frame   *get_sender_packet(uint32_t ssrc);
            uvg_rtp::frame::rtcp_receiver_frame *get_receiver_packet(uint32_t ssrc);
            uvg_rtp::frame::rtcp_sdes_frame     *get_sdes_packet(uint32_t ssrc);
            uvg_rtp::frame::rtcp_app_frame      *get_app_packet(uint32_t ssrc);

            /* create RTCP BYE packet using our own SSRC and send it to all participants */
            rtp_error_t terminate_self();

            /* Return a reference to vector that contains the sockets of all participants */
            std::vector<uvg_rtp::socket>& get_sockets();

            /* Somebody joined the multicast group the owner of this RTCP instance is part of
             * Add it to RTCP participant list so we can start listening for reports
             *
             * "clock_rate" tells how much the RTP timestamp advances, this information is needed
             * to calculate the interarrival jitter correctly. It has nothing do with our clock rate,
             * (or whether we're even sending anything)
             *
             * Return RTP_OK on success and RTP_ERROR on error */
            rtp_error_t add_participant(std::string dst_addr, uint16_t dst_port, uint16_t src_port, uint32_t clock_rate);

            /* Functions for updating various RTP sender statistics */
            void sender_inc_seq_cycle_count();
            void sender_inc_sent_pkts(size_t n);
            void sender_inc_sent_bytes(size_t n);
            void sender_update_stats(uvg_rtp::frame::rtp_frame *frame);

            void receiver_inc_sent_bytes(uint32_t sender_ssrc, size_t n);
            void receiver_inc_overhead_bytes(uint32_t sender_ssrc, size_t n);
            void receiver_inc_total_bytes(uint32_t sender_ssrc, size_t n);
            void receiver_inc_sent_pkts(uint32_t sender_ssrc, size_t n);

            /* Update the RTCP statistics regarding this packet
             *
             * Return RTP_OK if packet is valid
             * Return RTP_INVALID_VALUE if SSRCs of remotes have collided or the packet is invalid in some way
             * return RTP_SSRC_COLLISION if our own SSRC has collided and we need to reinitialize it */
            rtp_error_t receiver_update_stats(uvg_rtp::frame::rtp_frame *frame);

            /* If we've detected that our SSRC has collided with someone else's SSRC, we need to
             * generate new random SSRC and reinitialize our own RTCP state.
             * RTCP object still has the participants of "last session", we can use their SSRCs
             * to detected new collision
             *
             * Return RTP_OK if reinitialization succeeded
             * Return RTP_SSRC_COLLISION if our new SSRC has collided and we need to generate new SSRC */
            rtp_error_t reset_rtcp_state(uint32_t ssrc);

            /* Set wallclock reading for t = 0 and random RTP timestamp from where the counting is started
             * + clock rate for calculating the correct increment */
            void set_sender_ts_info(uint64_t clock_start, uint32_t clock_rate, uint32_t rtp_ts_start);

            /* Update various session statistics */
            void update_session_statistics(uvg_rtp::frame::rtp_frame *frame);

            /* Return SSRCs of all participants */
            std::vector<uint32_t> get_participants();

            /* Alternate way to get RTCP packets is to install a hook for them. So instead of
             * polling an RTCP packet, user can install a function that is called when
             * a specific RTCP packet is received. */
            rtp_error_t install_sender_hook(void (*hook)(uvg_rtp::frame::rtcp_sender_frame *));
            rtp_error_t install_receiver_hook(void (*hook)(uvg_rtp::frame::rtcp_receiver_frame *));
            rtp_error_t install_sdes_hook(void (*hook)(uvg_rtp::frame::rtcp_sdes_frame *));
            rtp_error_t install_app_hook(void (*hook)(uvg_rtp::frame::rtcp_app_frame *));

            /* Update RTCP-related session statistics */
            static rtp_error_t packet_handler(void *arg, int flags, frame::rtp_frame **out);

        private:
            static void rtcp_runner(rtcp *rtcp);

            /* when we start the RTCP instance, we don't know what the SSRC of the remote is
             * when an RTP packet is received, we must check if we've already received a packet
             * from this sender and if not, create new entry to receiver_stats_ map */
            bool is_participant(uint32_t ssrc);

            /* When we receive an RTP or RTCP packet, we need to check the source address and see if it's
             * the same address where we've received packets before.
             *
             * If the address is new, it means we have detected an SSRC collision and the paket should
             * be dropped We also need to check whether this SSRC matches with our own SSRC and if it does
             * we need to send RTCP BYE and rejoin to the session */
            bool collision_detected(uint32_t ssrc, sockaddr_in& src_addr);

            /* Move participant from initial_peers_ to participants_ */
            rtp_error_t add_participant(uint32_t ssrc);

            /* We've got a message from new source (the SSRC of the frame is not known to us)
             * Initialize statistics for the peer and move it to participants_ */
            rtp_error_t init_new_participant(uvg_rtp::frame::rtp_frame *frame);

            /* Initialize the RTP Sequence related stuff of peer
             * This function assumes that the peer already exists in the participants_ map */
            rtp_error_t init_participant_seq(uint32_t ssrc, uint16_t base_seq);

            /* Update the SSRC's sequence related data in participants_ map
             *
             * Return RTP_OK if the received packet was OK
             * Return RTP_GENERIC_ERROR if it wasn't and
             * packet-related statistics should not be updated */
            rtp_error_t update_participant_seq(uint32_t ssrc, uint16_t seq);

            /* Update the RTCP bandwidth variables
             *
             * "pkt_size" tells how much rtcp_byte_count_
             * should be increased before calculating the new average */
            void update_rtcp_bandwidth(size_t pkt_size);

            /* Functions for generating different kinds of reports.
             * These functions will both generate the report and send it
             *
             * Return RTP_OK on success and RTP_ERROR on error */
            rtp_error_t generate_sender_report();
            rtp_error_t generate_receiver_report();

            /* Because struct statistics contains uvgRTP clock object we cannot
             * zero it out without compiler complaining about it so all the fields
             * must be set to zero manually */
            void zero_stats(uvg_rtp::rtcp_statistics *stats);

            /* Pointer to RTP context from which clock rate etc. info is collected and which is
             * used to change SSRC if a collision is detected */
            uvg_rtp::rtp *rtp_;

            /* are we a sender or a receiver */
            int our_role_;

            /* TODO: time_t?? */
            size_t tp_;       /* the last time an RTCP packet was transmitted */
            size_t tc_;       /* the current time */
            size_t tn_;       /* the next scheduled transmission time of an RTCP packet */
            size_t pmembers_; /* the estimated number of session members at the time tn was last recomputed */
            size_t members_;  /* the most current estimate for the number of session members */
            size_t senders_;  /* the most current estimate for the number of senders in the session */

            /* The target RTCP bandwidth, i.e., the total bandwidth
             * that will be used for RTCP packets by all members of this session,
             * in octets per second.  This will be a specified fraction of the
             * "session bandwidth" parameter supplied to the application at startup. */
            size_t rtcp_bandwidth_;

            /* Flag that is true if the application has sent data since
             * the 2nd previous RTCP report was transmitted. */
            bool we_sent_;

            /* The average compound RTCP packet size, in octets,
             * over all RTCP packets sent and received by this participant. The
             * size includes lower-layer transport and network protocol headers
             * (e.g., UDP and IP) as explained in Section 6.2 */
            size_t avg_rtcp_pkt_pize_;

            /* Number of RTCP packets and bytes sent and received by this participant */
            size_t rtcp_pkt_count_;
            size_t rtcp_byte_count_;

            /* Flag that is true if the application has not yet sent an RTCP packet. */
            bool initial_;

            /* Copy of our own current SSRC */
            uint32_t ssrc_;

            /* NTP timestamp associated with initial RTP timestamp (aka t = 0) */
            uint64_t clock_start_;

            /* Clock rate of the media ie. how fast does the time increase */
            uint32_t clock_rate_;

            /* The first value of RTP timestamp (aka t = 0) */
            uint32_t rtp_ts_start_;

            std::map<uint32_t, rtcp_participant *> participants_;
            size_t num_receivers_;

            /* statistics for RTCP Sender and Receiver Reports */
            struct rtcp_statistics our_stats;

            /* If we expect frames from remote but haven't received anything from remote yet,
             * the participant resides in this vector until he's moved to participants_ */
            std::vector<rtcp_participant *> initial_participants_;

            /* Vector of sockets the RTCP runner is listening to
             *
             * The socket are also stored here (in addition to participants_ map) so they're easier
             * to pass to poll when RTCP runner is listening to incoming packets */
            std::vector<uvg_rtp::socket> sockets_;

            void (*sender_hook_)(uvg_rtp::frame::rtcp_sender_frame *);
            void (*receiver_hook_)(uvg_rtp::frame::rtcp_receiver_frame *);
            void (*sdes_hook_)(uvg_rtp::frame::rtcp_sdes_frame *);
            void (*app_hook_)(uvg_rtp::frame::rtcp_app_frame *);
    };
};
