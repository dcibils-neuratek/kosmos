/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **WebKit's connection between processes, on Nebula** (`docs/w7.md` W7a),
 * what `ConnectionUnix.cpp` is with a socket pair.
 *
 * A Nebula message is 2,048 bytes with one capability, and a call waits
 * for its answer. WebKit's connection sends both ways at once, without
 * waiting, messages of any size with any number of attachments. So it is
 * made of three of Nebula's parts:
 *
 * - **Two rings in one region**, one each way, single producer and single
 *   consumer: the side that made the connection writes ring 0, the other
 *   ring 1. A message is copied straight into the ring as a record - its
 *   length, its count of attachments, its bytes - and one larger than a
 *   quarter of a ring goes in a region of its own, read-only, sent as its
 *   last attachment, the ring carrying only the record (*Control by
 *   message, data by shared memory*).
 * - **A pulse when there is more**, sent only when the receiver had read
 *   everything before this record; a receiver still reading finds the
 *   next one without being told. The pulse's code is the same each time,
 *   so Nebula counts it rather than queueing it, and it cannot be refused.
 * - **An attachment is a capability, sent first**, by a call to the
 *   receiver. Its receiving thread queues it and answers at once, so the
 *   call waits only for the kernel's copy; then the record says how many
 *   it has. The calls come before the record, from the thread that writes
 *   it, so the receiver has them before it reads the record.
 *
 * **Each side receives on an endpoint the other side made.** An endpoint
 * goes with the process that made it, waking whatever waits on it with an
 * error, so a side learns its peer has ended from the receive it is
 * already blocked in. The pair is made in one process, so the other side's
 * receiving thread starts by calling the maker with a new endpoint of its
 * own (`tagInbox`), and the maker receives on that from then on; the other
 * side sends nothing until the call is answered. The maker's receiving
 * thread never calls, so two receiving threads cannot wait on each other.
 * Closing ends both endpoints a side uses, which wakes its own receiving
 * thread and its peer's.
 *
 * The ring's indices are counts of bytes that only grow, each written by
 * one side alone. The region is shared with another process, which may be
 * wrong or hostile, so every record is checked against the ring before it
 * is believed; one that does not fit ends the connection.
 */

#include "config.h"
#include "Connection.h"

#include "Decoder.h"
#include "Encoder.h"
#include "Logging.h"
#include <kosmos.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN // a ring of bytes in a region

namespace IPC {

namespace {

constexpr size_t ringBytes = 256 * 1024;
constexpr size_t controlBytes = KOSMOS_PAGE_SIZE;
constexpr size_t regionBytes = controlBytes + 2 * ringBytes;
constexpr size_t largestInline = ringBytes / 4;
constexpr size_t maximumAttachments = 254;

constexpr uint32_t pulseMore = 1;

constexpr uint64_t tagAttachment = 0x4b4f534d49504341ull; // an attachment
constexpr uint64_t tagInbox = 0x4b4f534d49504349ull; // the maker's inbox from now on
constexpr uint64_t tagRoom = 0x4b4f534d49504352ull; // the ring is full: answer when it is not

constexpr uint32_t wrapRecord = 0xffffffffu;
constexpr uint16_t outOfLine = 1;

struct Record {
    uint32_t bytes;
    uint16_t attachments;
    uint16_t flags;
};
static_assert(sizeof(Record) == 8);

struct alignas(64) Index {
    std::atomic<uint64_t> value;
};

// The region's first page: each ring's written count and read count, on
// lines of their own so the two sides do not share a cache line.
struct Control {
    Index written[2];
    Index read[2];
};
static_assert(sizeof(Control) <= controlBytes);

size_t pagesFor(size_t bytes)
{
    return (bytes + KOSMOS_PAGE_SIZE - 1) / KOSMOS_PAGE_SIZE;
}

size_t roundUp(size_t bytes)
{
    return (bytes + 7) & ~size_t { 7 };
}

} // namespace

static Control& control(uint8_t* rings)
{
    return *reinterpret_cast<Control*>(rings);
}

static uint8_t* ring(uint8_t* rings, int which)
{
    return rings + controlBytes + which * ringBytes;
}

void Connection::platformInitialize(Identifier&& identifier)
{
    m_nebula = WTF::move(identifier.handle);
}

void Connection::platformOpen()
{
    long address = m_nebula ? kosmos_mem_map(m_nebula.region().capability()) : -1;
    if (address < 0 || kosmos_mem_size(m_nebula.region().capability()) < static_cast<long>(pagesFor(regionBytes))) {
        RELEASE_LOG_ERROR(IPC, "Connection: the ring's region could not be mapped");
        return;
    }
    m_rings = reinterpret_cast<uint8_t*>(address);
    {
        Locker locker { m_nebulaLock };
        m_handshakeDone = m_nebula.maker();
    }
    m_isConnected = true;

    m_receiver = Thread::create("IPC receiver"_s, [protectedThis = Ref { *this }] {
        protectedThis->receiveOnNebula();
    });
}

void Connection::platformInvalidate()
{
    if (!m_isConnected)
        return;
    m_closing = true;

    // Both endpoints this side uses: the receiving thread here wakes from
    // its receive and stops, and the peer's wakes with the error a crash
    // would have given it. The maker's first inbox goes too, if the other
    // side never arrived to replace it; the other side's first outbox is
    // the maker's to end, and was, at the handshake.
    {
        Locker locker { m_nebulaLock };
        m_madeInbox.destroyEndpoint();
        m_handshakeDone = true; // a sender still waiting gives up
        m_handshakeCondition.notifyAll();
    }
    if (m_nebula.maker()) {
        m_nebula.inbox().destroyEndpoint();
        m_nebula.outbox().destroyEndpoint();
    } else
        m_nebula.inbox().destroyEndpoint();

    if (m_receiver) {
        m_receiver->detach();
        m_receiver = nullptr;
    }
    m_isConnected = false;
}

bool Connection::platformCanSendOutgoingMessages() const
{
    return true;
}

// The receiving thread: pulses, attachments and the handshake, until an
// endpoint goes.
void Connection::receiveOnNebula()
{
    long inbox = m_nebula.inbox().capability();

    if (!m_nebula.maker()) {
        long made = kosmos_endpoint();
        struct message hello { };
        struct message answer { };
        hello.tag = tagInbox;
        hello.cap_plus_one = made >= 0 ? static_cast<uint32_t>(made + 1) : 0;
        bool sent = made >= 0 && kosmos_call(m_nebula.outbox().capability(), &hello, &answer) >= 0;
        {
            Locker locker { m_nebulaLock };
            if (made >= 0)
                m_madeInbox = KosmosCapability::adopt(made);
            m_handshakeDone = true;
            m_handshakeCondition.notifyAll();
        }
        if (!sent) {
            if (!m_closing)
                connectionDidClose();
            return;
        }
    }

    bool retired = false;
    while (true) {
        struct message received;
        uint64_t sender = 0;
        long r = kosmos_receive(inbox, &received, &sender, 0, 0);

        if (m_closing)
            return;
        if (r < 0) {
            // The endpoint went: the peer ended, or closed.
            connectionDidClose();
            return;
        }

        if (received.tag == KOSMOS_PULSE_TAG) {
            if (!readRing()) {
                connectionDidClose();
                return;
            }
            continue;
        }

        long capability = received.cap_plus_one ? static_cast<long>(received.cap_plus_one) - 1 : -1;
        struct message answer { };

        switch (received.tag) {
        case tagAttachment:
            m_arrivedAttachments.append(KosmosCapability::adopt(capability));
            break;
        case tagInbox:
            if (m_nebula.maker() && capability >= 0) {
                Locker locker { m_nebulaLock };
                m_madeInbox = KosmosCapability::adopt(capability);
                inbox = capability;
                retired = true;
            } else if (capability >= 0)
                kosmos_cap_drop(capability);
            break;
        case tagRoom:
            if (!readRing()) {
                kosmos_reply(sender, &answer);
                connectionDidClose();
                return;
            }
            break;
        default:
            if (capability >= 0)
                kosmos_cap_drop(capability);
            break;
        }
        if (sender)
            kosmos_reply(sender, &answer);
        // The one this side received on until now is nobody's - ended only
        // once the call that replaced it is answered, since ending an
        // endpoint abandons every call still waiting on it, that one too.
        if (retired) {
            m_nebula.inbox().destroyEndpoint();
            retired = false;
        }
    }
}

// Every record the peer has written, handed to WebKit. False when one is
// not a record this side could have been sent.
bool Connection::readRing()
{
    int which = m_nebula.maker() ? 1 : 0;
    auto& written = control(m_rings).written[which].value;
    auto& read = control(m_rings).read[which].value;
    uint8_t* bytes = ring(m_rings, which);

    while (true) {
        uint64_t end = written.load();
        uint64_t at = read.load();
        if (at == end)
            return true;
        if (end - at > ringBytes || (at & 7))
            return false;

        size_t offset = at % ringBytes;
        Record record;
        memcpy(&record, bytes + offset, sizeof record);

        if (record.bytes == wrapRecord) {
            read.store(at + (ringBytes - offset));
            continue;
        }

        size_t inlineBytes = (record.flags & outOfLine) ? 0 : record.bytes;
        size_t length = sizeof record + roundUp(inlineBytes);
        if (length > end - at || offset + length > ringBytes || inlineBytes > largestInline
            || record.attachments > maximumAttachments || record.attachments > m_arrivedAttachments.size()
            || ((record.flags & outOfLine) && !record.attachments))
            return false;

        // **The decoder takes attachments from the end** (`takeLastAttachment`),
        // so the first one encoded has to be the last one handed to it - as
        // `ConnectionUnix.cpp` lays out its descriptors. They arrive here in
        // the order they were encoded, so they are reversed: a message with
        // one region could not tell, and with two - the rule lists a web
        // process is handed together (`docs/adblock.md` B2) - each list was
        // the other's bytes, and the matcher ran over JSON as bytecode.
        Vector<Attachment> attachments;
        attachments.reserveInitialCapacity(record.attachments);
        for (unsigned i = 0; i < record.attachments; ++i)
            attachments.append(m_arrivedAttachments.takeFirst());

        std::unique_ptr<Decoder> decoder;
        if (record.flags & outOfLine) {
            auto body = attachments.takeLast();
            long pages = kosmos_mem_size(body.capability());
            long address = pages > 0 ? kosmos_mem_map(body.capability()) : -1;
            if (address < 0 || static_cast<uint64_t>(record.bytes) > static_cast<uint64_t>(pages) * KOSMOS_PAGE_SIZE || !record.bytes)
                return false;
            attachments.reverse();
            decoder = Decoder::create(std::span { reinterpret_cast<const uint8_t*>(address), record.bytes }, WTF::move(attachments));
            kosmos_share_unmap(static_cast<unsigned long>(address), static_cast<unsigned long>(pages));
        } else {
            if (!record.bytes)
                return false;
            attachments.reverse();
            decoder = Decoder::create(std::span { bytes + offset + sizeof record, record.bytes }, WTF::move(attachments));
        }

        // Read before it is handed on: the ring's bytes were copied.
        read.store(at + length);

        if (!decoder)
            return false;
        processIncomingMessage(makeUniqueRefFromNonNullUniquePtr(WTF::move(decoder)));
    }
}

bool Connection::sendCapability(long outbox, uint64_t tag, long capability)
{
    struct message call { };
    struct message answer { };
    call.tag = tag;
    call.cap_plus_one = capability >= 0 ? static_cast<uint32_t>(capability + 1) : 0;
    return kosmos_call(outbox, &call, &answer) >= 0;
}

bool Connection::sendRecord(long outbox, std::span<const uint8_t> body, Vector<Attachment>&& attachments, bool bodyOutOfLine)
{
    int which = m_nebula.maker() ? 0 : 1;
    auto& written = control(m_rings).written[which].value;
    auto& read = control(m_rings).read[which].value;
    uint8_t* bytes = ring(m_rings, which);

    for (auto& attachment : attachments) {
        if (!sendCapability(outbox, tagAttachment, attachment.capability()))
            return false;
    }

    Record record { static_cast<uint32_t>(body.size()), static_cast<uint16_t>(attachments.size()),
        static_cast<uint16_t>(bodyOutOfLine ? outOfLine : 0) };
    size_t length = sizeof record + roundUp(bodyOutOfLine ? 0 : body.size());

    while (true) {
        uint64_t at = written.load();
        size_t offset = at % ringBytes;
        size_t wrap = offset + length > ringBytes ? ringBytes - offset : 0;

        if (wrap + length > ringBytes - (at - read.load())) {
            // Full: the receiving thread empties it, then answers.
            if (!sendCapability(outbox, tagRoom, -1))
                return false;
            continue;
        }

        if (wrap) {
            Record skip { wrapRecord, 0, 0 };
            memcpy(bytes + offset, &skip, sizeof skip);
            at += wrap;
            offset = 0;
        }
        memcpy(bytes + offset, &record, sizeof record);
        if (!bodyOutOfLine)
            memcpy(bytes + offset + sizeof record, body.data(), body.size());

        uint64_t before = written.load();
        written.store(at + length);
        // Pulsed only if the receiver had read all there was before this:
        // seen after the store, so either it sees this record, or this sees
        // that it had finished, or both.
        if (read.load() >= before && kosmos_pulse(outbox, pulseMore, 0) < 0)
            return false;
        return true;
    }
}

bool Connection::sendOutgoingMessage(UniqueRef<Encoder>&& encoder)
{
    if (!m_rings)
        return false;

    Locker locker { m_sendLock };
    long outbox;
    {
        Locker nebula { m_nebulaLock };
        while (!m_handshakeDone)
            m_handshakeCondition.wait(m_nebulaLock);
        outbox = m_nebula.maker() ? m_nebula.outbox().capability() : m_madeInbox.capability();
    }
    if (m_closing || outbox < 0)
        return false;

    auto attachments = encoder->releaseAttachments();
    auto body = encoder->span();
    if (attachments.size() >= maximumAttachments)
        return false;

    if (body.size() <= largestInline)
        return sendRecord(outbox, body, WTF::move(attachments), false);

    // A large message is a region of its own, read-only to the receiver.
    size_t pages = pagesFor(body.size());
    long region = kosmos_mem_create(pages);
    if (region < 0)
        return false;
    auto made = KosmosCapability::adopt(region);
    long address = kosmos_mem_map(region);
    if (address < 0)
        return false;
    memcpy(reinterpret_cast<void*>(address), body.data(), body.size());
    kosmos_share_unmap(static_cast<unsigned long>(address), pages);
    auto readOnly = KosmosCapability::adopt(kosmos_mem_readonly(region));
    if (!readOnly)
        return false;
    attachments.append(WTF::move(readOnly));
    return sendRecord(outbox, body, WTF::move(attachments), true);
}

std::optional<Connection::ConnectionIdentifierPair> Connection::createConnectionIdentifierPair()
{
    auto region = KosmosCapability::adopt(kosmos_mem_create(pagesFor(regionBytes)));
    auto first = KosmosCapability::adopt(kosmos_endpoint());
    auto second = KosmosCapability::adopt(kosmos_endpoint());
    if (!region || !first || !second)
        return std::nullopt;

    // The maker receives on the first and sends to the second; the other
    // side the other way round, until it hands the maker one of its own.
    return { { Identifier { ConnectionHandle { KosmosCapability { region }, KosmosCapability { first }, KosmosCapability { second }, true } },
        ConnectionHandle { WTF::move(region), WTF::move(second), WTF::move(first), false } } };
}

} // namespace IPC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
