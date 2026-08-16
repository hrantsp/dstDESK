// Little-endian byte order helpers.
//
// Both the dst wire format and RIFF are little-endian by definition, so these are
// shared rather than duplicated per format. Going byte by byte keeps them correct on
// a big-endian host without a branch at every call site.

#ifndef DST_DESK_CORE_ENDIAN_HPP
#define DST_DESK_CORE_ENDIAN_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>

namespace DST { namespace DESK { namespace Core {

inline std::uint16_t readU16(const std::byte* pp) noexcept
{
  return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(pp[0]) |
                                   (std::to_integer<std::uint16_t>(pp[1]) << 8));
}

inline std::uint32_t readU32(const std::byte* pp) noexcept
{
  return  std::to_integer<std::uint32_t>(pp[0])        |
         (std::to_integer<std::uint32_t>(pp[1]) <<  8) |
         (std::to_integer<std::uint32_t>(pp[2]) << 16) |
         (std::to_integer<std::uint32_t>(pp[3]) << 24);
}

inline void writeU16(std::byte* pp, std::uint16_t vv) noexcept
{
  pp[0] = static_cast<std::byte>( vv       & 0xFF);
  pp[1] = static_cast<std::byte>((vv >> 8) & 0xFF);
}

inline void writeU32(std::byte* pp, std::uint32_t vv) noexcept
{
  pp[0] = static_cast<std::byte>( vv        & 0xFF);
  pp[1] = static_cast<std::byte>((vv >>  8) & 0xFF);
  pp[2] = static_cast<std::byte>((vv >> 16) & 0xFF);
  pp[3] = static_cast<std::byte>((vv >> 24) & 0xFF);
}

inline void writeU16(std::ostream& out, std::uint16_t vv)
{
  auto bytes = std::array<std::byte, 2>{};
  writeU16(bytes.data(), vv);
  out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

inline void writeU32(std::ostream& out, std::uint32_t vv)
{
  auto bytes = std::array<std::byte, 4>{};
  writeU32(bytes.data(), vv);
  out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

} } } // namespace DST::DESK::Core

#endif // DST_DESK_CORE_ENDIAN_HPP
