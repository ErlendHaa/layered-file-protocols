#include <cassert>
#include <ciso646>
#include <limits>
#include <vector>

#include <fmt/format.h>

#include <lfp/protocol.hpp>
#include <lfp/rp66.h>

namespace lfp { namespace {

struct header {
    std::uint16_t  length;
    unsigned char  format;
    std::uint8_t   major;

    /*
     * Visible Records do not contain information about their own initial
     * offset into the file. That makes the mapping between physical- and
     * logical- offsets rather cumbersome. Calculating the offset of a record
     * can be quite expensive, as it's basically the sum of all previous record
     * lengths. Thus headers are augmented to include their physical offset.
     */
    std::int64_t offset = 0;

    /*
     * Reflects the *actual* number of bytes in the Visible Record Header,
     * defined here as the VE part of the VR. That is, Visible Record
     * Length and Format Version.
     */
    static constexpr const int size = 4;
};

/**
 * Address translator between physical offsets (provided by the underlying
 * layer) and logical offsets (presented to the user).
 */
class address_map {
public:
    address_map() = default;
    explicit address_map(std::int64_t z) : zero(z) {}

    /**
     * Get the logical address from the physical address, i.e. the one reported
     * by rp66::tell(), in the bytestream with no interleaved headers.
     */
    std::int64_t logical(std::int64_t addr, int record) const noexcept (true);
    /**
     * Get the physical address from the logical address, i.e. the address with
     * headers accounted for.
     *
     * Warning
     * -------
     *  This function assumes the physical address within record.
     */
    std::int64_t physical(std::int64_t addr, int record) const noexcept (true);

    /**
     * Base address of the map, i.e. the first possible address. This is
     * usually, but not guaranteed to be, zero.
     */
    std::int64_t base() const noexcept (true);

private:
    std::int64_t zero = 0;
};

/*
 * The record headers already read by rp66, stored in an order
 * (lower-address first fashion).
 */
class record_index : private std::vector< header > {
    using base = std::vector< header >;

public:
    using iterator = base::const_iterator;

    explicit record_index(address_map m);
    /*
     * Check if the logical address offset n is already indexed. If it is, then
     * find() will be defined, and return the correct record.
     */
    bool contains(std::int64_t n) const noexcept (true);

    /*
     * Find the record header that contains the logical offset n. Behaviour is
     * undefined if contains(n) is false.
     */
    iterator find(std::int64_t n) const noexcept (false);

    void append(const header& head) noexcept (false);

    iterator last() const noexcept (true);
    std::size_t size() const noexcept (true);
    bool empty() const noexcept (true);
    iterator begin() const noexcept (true);

    iterator::difference_type index_of(const iterator&) const noexcept (true);

private:
    address_map addr;
};

/**
 *
 * The read_head class implements part of the abstraction of a physical layer.
 * More precisely, it handles the state of the current record and the intricate
 * details of moving back and forth between Visible Records.
 *
 * It is somewhat flawed, as it is also an iterator over the record index,
 * which will trigger undefined behaviour when trying to obtain unindexed
 * records.
 */
class read_head : public record_index::iterator {
public:
    /*
     * true if the current record is exhausted. If this is true, then
     * bytes_left() == 0
     */
    bool exhausted() const noexcept (true);
    std::int64_t bytes_left() const noexcept (true);

    using base_type = record_index::iterator;
    read_head() = default;

    /*
     * Make a read head to a ghost node, i.e. the virtual header inserted into
     * the index *before* the first header, with its header->next pointing to
     * the offset of the first header in the file.
     */
    static read_head ghost(const base_type&) noexcept (true);

    /*
     * Move the read head within this record. Throws invalid_argument if n >
     * bytes_left
     */
    void move(std::int64_t n) noexcept (false);

    /*
     * Move the read head to the start of the record provided
     */
    void move(const base_type&) noexcept (true);

    /*
     * Skip to the end of this record. After skip(), exhausted() == true
     */
    void skip() noexcept (true);

    /*
     * Get a read head moved to the start of the next record. Behaviour is
     * undefined if this is the last record in the file.
     */
    read_head next_record() const noexcept (true);

    /*
     * The position of the read head. This should correspond to the offset
     * reported by the underlying file.
     */
    std::int64_t tell() const noexcept (true);

private:
    explicit read_head(const base_type& cur) : base_type(cur) {}
    std::int64_t remaining = -1;
};

class rp66 : public lfp_protocol {
public:
    rp66(lfp_protocol*);

    // TODO: there must be a "reset" semantic for when there's a read error to
    // put it back into a valid state

    void close() noexcept (false) override;
    lfp_status readinto(void* dst, std::int64_t len, std::int64_t* bytes_read)
        noexcept (false) override;

    int eof() const noexcept (true) override;
    std::int64_t tell() const noexcept (true) override;
    void seek(std::int64_t) noexcept (false) override;
    lfp_protocol* peel() noexcept (false) override;
    lfp_protocol* peek() const noexcept (false) override;

private:
    unique_lfp fp;
    address_map addr;
    record_index index;
    read_head current;

    std::int64_t readinto(void*, std::int64_t) noexcept (false);
    void read_header_from_disk() noexcept (false);
};

std::int64_t
address_map::logical(std::int64_t addr, int record)
const noexcept (true) {
    return addr - (header::size * (1 + record)) - this->zero;
}

std::int64_t
address_map::physical(std::int64_t addr, int record)
const noexcept (true) {
    return addr + (header::size * (1 + record)) + this->zero;
}

std::int64_t address_map::base() const noexcept (true) {
    return this->zero;
}

record_index::record_index(address_map m) : addr(m) {
    header ghost;

    /**
     * "Insert" the ghost node right before the first actual header.
     *
     * For the ghost node to be truly invisible we need to make sure base +
     * length == this->addr.base() as this is what the next (first actual)
     * header uses to set it's base.
     *
     * The values for format and major are set so that the ghost would never be
     * accepted as a real header.
     */
    ghost.length = header::size;
    ghost.offset = this->addr.base() - ghost.length;
    ghost.format = 0x00;
    ghost.major = 255;
    this->append(ghost);
}

bool record_index::contains(std::int64_t n) const noexcept (true) {
    const auto last = this->last();
    return n < this->addr.logical(last->offset + last->length, this->size());
}

record_index::iterator
record_index::find(std::int64_t n) const noexcept (false) {
    assert(this->contains(n));

    auto cur = this->begin();
    while (true) {
        const auto pos = this->index_of(cur);
        const auto off = cur->offset + cur->length;

        if (n < this->addr.logical(off, pos))
            return cur;

        cur++;
    }
}

void record_index::append(const header& head) noexcept (false) {
    try {
        this->push_back(head);
    } catch (...) {
        throw runtime_error("rp66: unable to store header");
    }
}

record_index::iterator
record_index::last() const noexcept (true) {
    return std::prev(this->end());
}

std::size_t record_index::size() const noexcept (true) {
    return this->base::size() - 1;
}

bool record_index::empty() const noexcept (true) {
    return this->size() == 0;
}

record_index::iterator record_index::begin() const noexcept (true) {
    return this->base::begin() + 1;
}

record_index::iterator::difference_type
record_index::index_of(const iterator& itr) const noexcept (true) {
    return std::distance(this->begin(), itr);
}

read_head read_head::ghost(const base_type& b) noexcept (true) {
    auto x = read_head(b);
    x.remaining = 0;
    return x;
}

bool read_head::exhausted() const noexcept (true) {
    assert(this->remaining >= 0);
    return this->remaining == 0;
}

std::int64_t read_head::bytes_left() const noexcept (true) {
    assert(this->remaining >= 0);
    return this->remaining;
}

void read_head::move(std::int64_t n) noexcept (false) {
    assert(n >= 0);
    assert(this->remaining >= 0);
    if (this->remaining - n < 0)
        throw std::invalid_argument("advancing read_head past end-of-record");

    this->remaining -= n;
}

void read_head::move(const base_type& itr) noexcept (true) {
    assert(this->remaining >= 0);
    /*
     * This is carefully implemented not to reference any this-> members, as
     * the underlying iterator may have been invalidated by an index append.
     * move() is the correct way to position the read_head in a new record.
     */
    read_head copy(itr);
    copy.remaining = copy->length - header::size;
    *this = copy;
}

void read_head::skip() noexcept (true) {
    assert(this->remaining >= 0);
    this->remaining = 0;
}

read_head read_head::next_record() const noexcept (true) {
    assert(this->remaining >= 0);
    auto next = *this;
    next.move(std::next(*this));
    return next;
}

std::int64_t read_head::tell() const noexcept (true) {
    assert(this->remaining >= 0);
    return (*this)->offset + (*this)->length - this->remaining;
}

std::int64_t baseaddr(lfp_protocol* f) noexcept (true) {
    try {
        return f->tell();
    } catch (const lfp::error&) {
        return 0;
    }
}

rp66::rp66(lfp_protocol* f) :
    fp(f),
    addr(baseaddr(f)),
    index(this->addr)
{
    this->current = read_head::ghost(this->index.last());
}

void rp66::close() noexcept (false) {
    if(!this->fp) return;
    this->fp.close();
}

lfp_protocol* rp66::peel() noexcept (false) {
    assert(this->fp);
    return this->fp.release();
}

lfp_protocol* rp66::peek() const noexcept (false) {
    assert(this->fp);
    return this->fp.get();
}

lfp_status rp66::readinto(
        void* dst,
        std::int64_t len,
        std::int64_t* bytes_read)
noexcept (false) {
    const auto n = this->readinto(dst, len);
    assert(n <= len);

    if (bytes_read) *bytes_read = n;

    if (n == len)
        return LFP_OK;

    if (this->eof())
        return LFP_EOF;
    else
        return LFP_OKINCOMPLETE;
}

int rp66::eof() const noexcept (true) {
    /*
     * There is no trailing header information. I.e. the end of the last
     * Visible Record *should* align with EOF from the underlying file handle.
     * If not, the VR is either truncated or there are some garbage bytes at
     * the end.
     */
    return this->fp->eof();
}

std::int64_t rp66::tell() const noexcept (true) {
    const auto pos = this->index.index_of(this->current);
    return this->addr.logical(this->current.tell(), pos);
}

void rp66::seek(std::int64_t n) noexcept (false) {
    /*
     * Have we already index'd the right section? If so, use it and seek there.
     */

    if (this->index.contains(n)) {
        const auto next = this->index.find(n);
        const auto pos  = this->index.index_of(next);
        const auto real_offset = this->addr.physical(n, pos);

        this->fp->seek(real_offset);
        this->current.move(next);
        this->current.move(real_offset - this->current.tell());
        return;
    }
    /*
     * target is past the already-index'd records, so follow the headers, and
     * index them as we go
     */
    this->current.move(this->index.last());
    while (true) {
        const auto last = this->index.last();
        const auto pos  = this->index.index_of(last);
        const auto real_offset = this->addr.physical(n, pos);
        const auto end = last->offset + last->length;

        if (real_offset < end) {
            this->fp->seek(real_offset);
            this->current.move(real_offset - this->current.tell());
            return;
        }

        if (real_offset == end) {
            this->fp->seek(end);
            this->current.skip();
            return;
        }

        this->current.skip();
        this->fp->seek(end);
        this->read_header_from_disk();
        if (this->eof()) return;
        this->current.move(this->index.last());
    }
}

std::int64_t rp66::readinto(void* dst, std::int64_t len) noexcept (false) {
    assert(this->current.bytes_left() >= 0);
    std::int64_t bytes_read = 0;

    while (true) {
        if (this->eof())
            return bytes_read;
        if (this->current.exhausted()) {
            if (this->current == this->index.last()) {
                this->read_header_from_disk();
                if (this->eof()) return bytes_read;
                this->current.move(this->index.last());
            } else {
                const auto next = this->current.next_record();
                this->fp->seek(next.tell());
                this->current.move(next);
            }
            /* might be EOF, or even empty records, so re-start  */
            continue;
        }

        assert(not this->current.exhausted());
        std::int64_t n;
        const auto to_read = std::min(len, this->current.bytes_left());
        const auto err = this->fp->readinto(dst, to_read, &n);

        this->current.move(n);
        bytes_read += n;
        dst = advance(dst, n);

        if (err == LFP_OKINCOMPLETE)
            return bytes_read;

        if (err == LFP_EOF and not this->current.exhausted()) {
            const auto msg = "rp66: unexpected EOF when reading record "
                             "- got {} bytes, expected there to be {} more";
            throw unexpected_eof(fmt::format(msg, n, this->current.bytes_left()));
        }

        if (err == LFP_EOF and this->current.exhausted())
            return bytes_read;

        assert(err == LFP_OK);

        if (n == len)
            return bytes_read;
        /*
         * The full read was performed, but there's still more requested - move
         * onto the next segment. This differs from when read returns OKINCOMPLETE,
         * in which case the underlying stream is temporarily exhausted or blocked,
         * and fewer bytes than requested could be provided.
         */

        len -= n;
    }
}

void rp66::read_header_from_disk() noexcept (false) {
    assert(this->current == this->index.last() and this->current.exhausted());

    std::int64_t n;
    unsigned char b[header::size];
    auto err = this->fp->readinto(b, sizeof(b), &n);
    switch (err) {
        case LFP_OK: break;

        case LFP_OKINCOMPLETE:
            throw protocol_failed_recovery(
                "rp66: incomplete read of Visible Record Header, "
                "recovery not implemented"
            );
        case LFP_EOF:
            /*
             * The end of the *last* Visible Record aligns perfectly with
             * EOF as there are no trailing bytes. Because EOF are typically
             * not recorded before someone tries to read *past* the end, its
             * perfectly fine to exhaust the last VR without EOF being set.
             */
            if (n == 0)
                return;
            else {
                const auto msg = "rp66: unexpected EOF when reading header "
                                 "- got {} bytes";
                throw protocol_fatal(fmt::format(msg, n));
            }


        default:
            throw not_implemented(
                "rp66: unhandled error code in read_header_from_disk"
            );
    }

    // Check the makefile-provided IS_LITTLE_ENDIAN, or the one set by gcc
    #if (defined(IS_LITTLE_ENDIAN) || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        std::reverse(b + 0, b + 2);
    #endif

    header head;

    std::memcpy(&head.length, b + 0, sizeof(head.length));
    std::memcpy(&head.format, b + 2, sizeof(head.format));
    std::memcpy(&head.major,  b + 3, sizeof(head.major));

    /*
     * rp66v1 defines that the Format Version should _always_ be [0xFF 0x01].
     * Currently there are no other know applications of Visible Envelope (not
     * to be confused with rp66v2's Visible Record, which is a different
     * format). We therefore make this a strict requirement in the hopes that
     * it will help identify broken- and none VE files.
     */
    if (head.format != 0xFF or head.major != 1) {
        const auto msg = "rp66: Incorrect format version in Visible Record {}";
        throw protocol_fatal( fmt::format(msg, this->index.size() + 1) );
    }

    std::int64_t base = this->addr.base();
    if ( !this->index.empty() ) {
        base = this->index.last()->offset + this->index.last()->length;
    }
    head.offset = base;

    this->index.append(head);
}

}

}

lfp_protocol* lfp_rp66_open(lfp_protocol* f) {
    if (not f) return nullptr;

    try {
        return new lfp::rp66(f);
    } catch (...) {
        return nullptr;
    }
}
