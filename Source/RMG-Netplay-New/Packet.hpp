/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef RMG_NETPLAY_NEW_PACKET_HPP
#define RMG_NETPLAY_NEW_PACKET_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace NetplayNew
{
// Builds a message. Everything is little-endian.
class PacketWriter
{
  public:
    PacketWriter& U8(uint8_t value)
    {
        this->data.push_back(value);
        return *this;
    }

    PacketWriter& U16(uint16_t value)
    {
        return this->Uint(value, 2);
    }

    PacketWriter& U32(uint32_t value)
    {
        return this->Uint(value, 4);
    }

    PacketWriter& I64(int64_t value)
    {
        return this->Uint(static_cast<uint64_t>(value), 8);
    }

    PacketWriter& String(const std::string& value)
    {
        this->U16(static_cast<uint16_t>(std::min<size_t>(value.size(), 0xFFFF)));
        this->data.insert(this->data.end(), value.begin(), value.begin() + std::min<size_t>(value.size(), 0xFFFF));
        return *this;
    }

    PacketWriter& Bytes(const void* bytes, size_t size)
    {
        this->U32(static_cast<uint32_t>(size));
        const uint8_t* p = static_cast<const uint8_t*>(bytes);
        this->data.insert(this->data.end(), p, p + size);
        return *this;
    }

    PacketWriter& Bytes(const std::vector<uint8_t>& bytes)
    {
        return this->Bytes(bytes.data(), bytes.size());
    }

    size_t Size(void) const
    {
        return this->data.size();
    }

    const std::vector<uint8_t>& Data(void) const
    {
        return this->data;
    }

    // overwrites a byte that was written earlier (used for counts)
    void Patch(size_t offset, uint8_t value)
    {
        this->data.at(offset) = value;
    }

  private:
    std::vector<uint8_t> data;

    PacketWriter& Uint(uint64_t value, int bytes)
    {
        for (int i = 0; i < bytes; i++)
        {
            this->data.push_back(static_cast<uint8_t>(value >> (i * 8)));
        }
        return *this;
    }
};

// Reads a message. Throws std::runtime_error when the message is cut short.
class PacketReader
{
  public:
    PacketReader(const uint8_t* data, size_t size) : data(data), size(size)
    {
    }

    uint8_t U8(void)
    {
        this->Need(1);
        return this->data[this->offset++];
    }

    uint16_t U16(void)
    {
        return static_cast<uint16_t>(this->Uint(2));
    }

    uint32_t U32(void)
    {
        return static_cast<uint32_t>(this->Uint(4));
    }

    int64_t I64(void)
    {
        return static_cast<int64_t>(this->Uint(8));
    }

    std::string String(void)
    {
        uint16_t length = this->U16();
        this->Need(length);
        std::string value(reinterpret_cast<const char*>(this->data + this->offset), length);
        this->offset += length;
        return value;
    }

    std::vector<uint8_t> Bytes(void)
    {
        uint32_t length = this->U32();
        this->Need(length);
        std::vector<uint8_t> value(this->data + this->offset, this->data + this->offset + length);
        this->offset += length;
        return value;
    }

    size_t Remaining(void) const
    {
        return this->size - this->offset;
    }

  private:
    const uint8_t* data;
    size_t size;
    size_t offset = 0;

    void Need(size_t bytes)
    {
        if (this->size - this->offset < bytes)
        {
            throw std::runtime_error("message is cut short");
        }
    }

    uint64_t Uint(int bytes)
    {
        this->Need(bytes);
        uint64_t value = 0;
        for (int i = 0; i < bytes; i++)
        {
            value |= static_cast<uint64_t>(this->data[this->offset++]) << (i * 8);
        }
        return value;
    }
};
} // namespace NetplayNew

#endif // RMG_NETPLAY_NEW_PACKET_HPP
