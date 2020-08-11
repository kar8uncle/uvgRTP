#ifdef _WIN32
#else
#include <sys/time.h>
#endif

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "debug.hh"
#include "hostname.hh"
#include "poll.hh"
#include "rtcp.hh"
#include "util.hh"

#define UDP_HEADER_SIZE  8
#define IP_HEADER_SIZE  20

uvg_rtp::rtcp::rtcp(uvg_rtp::rtp *rtp):
    rtp_(rtp), our_role_(RECEIVER),
    tp_(0), tc_(0), tn_(0), pmembers_(0),
    members_(0), senders_(0), rtcp_bandwidth_(0),
    we_sent_(0), avg_rtcp_pkt_pize_(0), rtcp_pkt_count_(0),
    initial_(true), num_receivers_(0)
{
    ssrc_         = rtp->get_ssrc();
    clock_rate_   = rtp->get_clock_rate();

    clock_start_  = 0;
    rtp_ts_start_ = 0;
    runner_       = nullptr;

    zero_stats(&our_stats);
}

uvg_rtp::rtcp::~rtcp()
{
}

rtp_error_t uvg_rtp::rtcp::start()
{
    if (sockets_.empty()) {
        LOG_ERROR("Cannot start RTCP Runner because no connections have been initialized");
        return RTP_INVALID_VALUE;
    }
    active_ = true;

    if (!(runner_ = new std::thread(rtcp_runner, this))) {
        active_ = false;
        LOG_ERROR("Failed to create RTCP thread!");
        return RTP_MEMORY_ERROR;
    }
    runner_->detach();

    return RTP_OK;
}

rtp_error_t uvg_rtp::rtcp::stop()
{
    if (!runner_)
        goto free_mem;

    /* when the member count is less than 50,
     * we can just send the BYE message and destroy the session */
    if (members_ < 50) {
        active_ = false;
        goto end;
    }

    tp_       = tc_;
    members_  = 1;
    pmembers_ = 1;
    initial_  = true;
    we_sent_  = false;
    senders_  = 0;
    active_   = false;

end:
    /* Send BYE packet with our SSRC to all participants */
    uvg_rtp::rtcp::terminate_self();

free_mem:
    /* free all receiver statistic structs */
    for (auto& participant : participants_) {
        delete participant.second->socket;
        delete participant.second;
    }

    return RTP_OK;
}

rtp_error_t uvg_rtp::rtcp::add_participant(std::string dst_addr, uint16_t dst_port, uint16_t src_port, uint32_t clock_rate)
{
    if (dst_addr == "" || !dst_port || !src_port) {
        LOG_ERROR("Invalid values given (%s, %d, %d), cannot create RTCP instance",
                dst_addr.c_str(), dst_port, src_port);
        return RTP_INVALID_VALUE;
    }

    rtp_error_t ret;
    rtcp_participant *p;

    if (!(p = new rtcp_participant))
        return RTP_MEMORY_ERROR;

    zero_stats(&p->stats);

    if (!(p->socket = new uvg_rtp::socket(RTP_CTX_NO_FLAGS)))
        return RTP_MEMORY_ERROR;

    if ((ret = p->socket->init(AF_INET, SOCK_DGRAM, 0)) != RTP_OK)
        return ret;

    int enable = 1;

    if ((ret = p->socket->setsockopt(SOL_SOCKET, SO_REUSEADDR, (const char *)&enable, sizeof(int))) != RTP_OK)
        return ret;

#ifdef _WIN32
    /* Make the socket non-blocking */
    int enabled = 1;

    if (::ioctlsocket(p->socket->get_raw_socket(), FIONBIO, (u_long *)&enabled) < 0)
        LOG_ERROR("Failed to make the socket non-blocking!");
#endif

    /* Set read timeout (5s for now)
     *
     * This means that the socket is listened for 5s at a time and after the timeout,
     * Send Report is sent to all participants */
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;

    if ((ret = p->socket->setsockopt(SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv))) != RTP_OK)
        return ret;

    LOG_WARN("Binding to port %d (source port)", src_port);

    if ((ret = p->socket->bind(AF_INET, INADDR_ANY, src_port)) != RTP_OK)
        return ret;

    p->role             = RECEIVER;
    p->address          = p->socket->create_sockaddr(AF_INET, dst_addr, dst_port);
    p->stats.clock_rate = clock_rate;

    initial_participants_.push_back(p);
    sockets_.push_back(*p->socket);

    return RTP_OK;
}

rtp_error_t uvg_rtp::rtcp::add_participant(uint32_t ssrc)
{
    /* RTCP is not in use for this media stream,
     * create a "fake" participant that is only used for storing statistics information */
    if (initial_participants_.empty()) {
        if (!(participants_[ssrc] = new rtcp_participant))
            return RTP_MEMORY_ERROR;
        zero_stats(&participants_[ssrc]->stats);
    } else {
        participants_[ssrc] = initial_participants_.back();
        initial_participants_.pop_back();
    }
    num_receivers_++;

    participants_[ssrc]->r_frame    = nullptr;
    participants_[ssrc]->s_frame    = nullptr;
    participants_[ssrc]->sdes_frame = nullptr;
    participants_[ssrc]->app_frame  = nullptr;

    return RTP_OK;
}

void uvg_rtp::rtcp::update_rtcp_bandwidth(size_t pkt_size)
{
    rtcp_pkt_count_    += 1;
    rtcp_byte_count_   += pkt_size + UDP_HEADER_SIZE + IP_HEADER_SIZE;
    avg_rtcp_pkt_pize_  = rtcp_byte_count_ / rtcp_pkt_count_;
}

void uvg_rtp::rtcp::zero_stats(uvg_rtp::rtcp_statistics *stats)
{
    stats->received_pkts  = 0;
    stats->dropped_pkts   = 0;
    stats->received_bytes = 0;

    stats->sent_pkts  = 0;
    stats->sent_bytes = 0;

    stats->jitter  = 0;
    stats->transit = 0;

    stats->initial_ntp = 0;
    stats->initial_rtp = 0;
    stats->clock_rate  = 0;
    stats->lsr         = 0;

    stats->max_seq  = 0;
    stats->base_seq = 0;
    stats->bad_seq  = 0;
    stats->cycles   = 0;
}

bool uvg_rtp::rtcp::is_participant(uint32_t ssrc)
{
    return participants_.find(ssrc) != participants_.end();
}

void uvg_rtp::rtcp::set_sender_ts_info(uint64_t clock_start, uint32_t clock_rate, uint32_t rtp_ts_start)
{
    clock_start_  = clock_start;
    clock_rate_   = clock_rate;
    rtp_ts_start_ = rtp_ts_start;
}

void uvg_rtp::rtcp::sender_update_stats(uvg_rtp::frame::rtp_frame *frame)
{
    if (!frame)
        return;

    our_stats.sent_pkts  += 1;
    our_stats.sent_bytes += frame->payload_len;
    our_stats.max_seq     = frame->header.seq;
}

rtp_error_t uvg_rtp::rtcp::init_new_participant(uvg_rtp::frame::rtp_frame *frame)
{
    rtp_error_t ret;

    if ((ret = uvg_rtp::rtcp::add_participant(frame->header.ssrc)) != RTP_OK)
        return ret;

    if ((ret = uvg_rtp::rtcp::init_participant_seq(frame->header.ssrc, frame->header.seq)) != RTP_OK)
        return ret;

    /* Set the probation to MIN_SEQUENTIAL (2)
     *
     * What this means is that we must receive at least two packets from SSRC
     * with sequential RTP sequence numbers for this peer to be considered valid */
    participants_[frame->header.ssrc]->probation = MIN_SEQUENTIAL;

    /* This is the first RTP frame from remote to frame->header.timestamp represents t = 0
     * Save the timestamp and current NTP timestamp so we can do jitter calculations later on */
    participants_[frame->header.ssrc]->stats.initial_rtp = frame->header.timestamp;
    participants_[frame->header.ssrc]->stats.initial_ntp = uvg_rtp::clock::ntp::now();

    senders_++;

    return ret;
}

rtp_error_t uvg_rtp::rtcp::init_participant_seq(uint32_t ssrc, uint16_t base_seq)
{
    if (participants_.find(ssrc) == participants_.end())
        return RTP_NOT_FOUND;

    participants_[ssrc]->stats.base_seq = base_seq;
    participants_[ssrc]->stats.max_seq  = base_seq;
    participants_[ssrc]->stats.bad_seq  = (uint16_t)RTP_SEQ_MOD + 1;

    return RTP_OK;
}

rtp_error_t uvg_rtp::rtcp::update_participant_seq(uint32_t ssrc, uint16_t seq)
{
    if (participants_.find(ssrc) == participants_.end())
        return RTP_GENERIC_ERROR;

    auto p = participants_[ssrc];
    uint16_t udelta = seq - p->stats.max_seq;

    /* Source is not valid until MIN_SEQUENTIAL packets with
    * sequential sequence numbers have been received.  */
    if (p->probation) {
       /* packet is in sequence */
       if (seq == p->stats.max_seq + 1) {
           p->probation--;
           p->stats.max_seq = seq;
           if (!p->probation) {
               uvg_rtp::rtcp::init_participant_seq(ssrc, seq);
               return RTP_OK;
            }
       } else {
           p->probation = MIN_SEQUENTIAL - 1;
           p->stats.max_seq = seq;
       }
       return RTP_GENERIC_ERROR;
    } else if (udelta < MAX_DROPOUT) {
       /* in order, with permissible gap */
       if (seq < p->stats.max_seq) {
           /* Sequence number wrapped - count another 64K cycle.  */
           p->stats.cycles += RTP_SEQ_MOD;
       }
       p->stats.max_seq = seq;
    } else if (udelta <= RTP_SEQ_MOD - MAX_MISORDER) {
       /* the sequence number made a very large jump */
       if (seq == p->stats.bad_seq) {
           /* Two sequential packets -- assume that the other side
            * restarted without telling us so just re-sync
            * (i.e., pretend this was the first packet).  */
           uvg_rtp::rtcp::init_participant_seq(ssrc, seq);
       }
       else {
           p->stats.bad_seq = (seq + 1) & (RTP_SEQ_MOD - 1);
           return RTP_GENERIC_ERROR;
       }
    } else {
       /* duplicate or reordered packet */
    }

    return RTP_OK;
}

rtp_error_t uvg_rtp::rtcp::terminate_self()
{
    rtp_error_t ret;
    auto bye_frame = uvg_rtp::frame::alloc_rtcp_bye_frame(1);

    bye_frame->ssrc[0] = ssrc_;

    if ((ret = send_bye_packet(bye_frame)) != RTP_OK) {
        LOG_ERROR("Failed to send BYE");
    }

    (void)uvg_rtp::frame::dealloc_frame(bye_frame);

    return ret;
}

rtp_error_t uvg_rtp::rtcp::reset_rtcp_state(uint32_t ssrc)
{
    if (participants_.find(ssrc) != participants_.end())
        return RTP_SSRC_COLLISION;

    our_stats.received_pkts  = 0;
    our_stats.dropped_pkts   = 0;
    our_stats.received_bytes = 0;
    our_stats.sent_pkts      = 0;
    our_stats.sent_bytes     = 0;
    our_stats.jitter         = 0;
    our_stats.transit        = 0;
    our_stats.max_seq        = 0;
    our_stats.base_seq       = 0;
    our_stats.bad_seq        = 0;
    our_stats.cycles         = 0;

    return RTP_OK;
}

bool uvg_rtp::rtcp::collision_detected(uint32_t ssrc, sockaddr_in& src_addr)
{
    if (participants_.find(ssrc) == participants_.end())
        return false;

    auto sender = participants_[ssrc];

    if (src_addr.sin_port        != sender->address.sin_port &&
        src_addr.sin_addr.s_addr != sender->address.sin_addr.s_addr)
        return true;

    return false;
}

void uvg_rtp::rtcp::update_session_statistics(uvg_rtp::frame::rtp_frame *frame)
{
    auto p = participants_[frame->header.ssrc];

    p->stats.received_pkts  += 1;
    p->stats.received_bytes += frame->payload_len;

    /* calculate number of dropped packets */
    int extended_max = p->stats.cycles + p->stats.max_seq;
    int expected     = extended_max - p->stats.base_seq + 1;

    p->stats.dropped_pkts = expected - p->stats.received_pkts;

    int arrival =
        p->stats.initial_rtp +
        + uvg_rtp::clock::ntp::diff_now(p->stats.initial_ntp)
        * (p->stats.clock_rate
        / 1000);

	/* calculate interarrival jitter */
    int transit = arrival - frame->header.timestamp;
    int d = std::abs((int)(transit - p->stats.transit));

    p->stats.transit = transit;
    p->stats.jitter += (1.f / 16.f) * ((double)d - p->stats.jitter);
}

/* RTCP packet handler is responsible for doing two things:
 *
 * - it checks whether the packet is coming from an existing user and if so,
 *   updates that user's session statistics. If the packet is coming from a user,
 *   the user is put on probation where they will stay until enough valid packets
 *   have been received.
 * - it keeps track of participants' SSRCs and if a collision
 *   is detected, the RTP context is updated */
rtp_error_t uvg_rtp::rtcp::packet_handler(void *arg, int flags, frame::rtp_frame **out)
{
    (void)flags;

    rtp_error_t ret;
    uvg_rtp::frame::rtp_frame *frame = *out;
    uvg_rtp::rtcp *rtcp              = (uvg_rtp::rtcp *)arg;

    /* If this is the first packet from remote, move the participant from initial_participants_
     * to participants_, initialize its state and put it on probation until enough valid
     * packets from them have been received
     *
     * Otherwise update and monitor the received sequence numbers to determine whether something
     * has gone awry with the sender's sequence number calculations/delivery of packets */
    if (!rtcp->is_participant(frame->header.ssrc)) {
        if ((ret = rtcp->init_new_participant(frame)) != RTP_OK)
            return RTP_GENERIC_ERROR;
    } else if (rtcp->update_participant_seq(frame->header.ssrc, frame->header.seq) != RTP_OK) {
        return RTP_GENERIC_ERROR;
    }

    /* Finally update the jitter/transit/received/dropped bytes/pkts statistics */
    rtcp->update_session_statistics(frame);

    /* Even though RTCP collects information from the packet, this is not the packet's final destination.
     * Thus return RTP_PKT_NOT_HANDLED to indicate that the packet should be passed on to other handlers */
    return RTP_PKT_NOT_HANDLED;
}

rtp_error_t uvg_rtp::rtcp::handle_incoming_packet(uint8_t *buffer, size_t size)
{
    (void)size;

    uvg_rtp::frame::rtcp_header *header = (uvg_rtp::frame::rtcp_header *)buffer;

    if (header->version != 2) {
        LOG_ERROR("Invalid header version (%u)", header->version);
        return RTP_INVALID_VALUE;
    }

    if (header->padding) {
        LOG_ERROR("Cannot handle padded packets!");
        return RTP_INVALID_VALUE;
    }

    if (header->pkt_type > uvg_rtp::frame::RTCP_FT_BYE ||
        header->pkt_type < uvg_rtp::frame::RTCP_FT_SR) {
        LOG_ERROR("Invalid packet type (%u)!", header->pkt_type);
        return RTP_INVALID_VALUE;
    }

    update_rtcp_bandwidth(size);

    rtp_error_t ret = RTP_INVALID_VALUE;

    switch (header->pkt_type) {
        case uvg_rtp::frame::RTCP_FT_SR:
            ret = handle_sender_report_packet((uvg_rtp::frame::rtcp_sender_frame *)buffer, size);
            break;

        case uvg_rtp::frame::RTCP_FT_RR:
            ret = handle_receiver_report_packet((uvg_rtp::frame::rtcp_receiver_frame *)buffer, size);
            break;

        case uvg_rtp::frame::RTCP_FT_SDES:
            ret = handle_sdes_packet((uvg_rtp::frame::rtcp_sdes_frame *)buffer, size);
            break;

        case uvg_rtp::frame::RTCP_FT_BYE:
            ret = handle_bye_packet((uvg_rtp::frame::rtcp_bye_frame *)buffer, size);
            break;

        case uvg_rtp::frame::RTCP_FT_APP:
            ret = handle_app_packet((uvg_rtp::frame::rtcp_app_frame *)buffer, size);
            break;

        default:
            LOG_WARN("Unknown packet received, type %d", header->pkt_type);
            break;
    }

    return ret;
}
