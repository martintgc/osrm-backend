#ifndef OSMIUM_IO_BZIP2_COMPRESSION_HPP
#define OSMIUM_IO_BZIP2_COMPRESSION_HPP

/*

This file is part of Osmium (http://osmcode.org/libosmium).

Copyright 2013-2016 Jochen Topf <jochen@topf.org> and others (see README).

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

*/

/**
 * @file
 *
 * Include this file if you want to read or write bzip2-compressed OSM XML
 * files.
 *
 * @attention If you include this file, you'll need to link with `libbz2`.
 */

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string>

#include <bzlib.h>

#ifndef _MSC_VER
# include <unistd.h>
#endif

#include <osmium/io/compression.hpp>
#include <osmium/io/error.hpp>
#include <osmium/io/file_compression.hpp>
#include <osmium/io/writer_options.hpp>
#include <osmium/util/cast.hpp>
#include <osmium/util/compatibility.hpp>

namespace osmium {

    /**
     * Exception thrown when there are problems compressing or
     * decompressing bzip2 files.
     */
    struct bzip2_error : public io_error {

        int bzip2_error_code;
        int system_errno;

        bzip2_error(const std::string& what, int error_code) :
            io_error(what),
            bzip2_error_code(error_code),
            system_errno(error_code == BZ_IO_ERROR ? errno : 0) {
        }

    }; // struct bzip2_error

    namespace io {

        namespace detail {

            OSMIUM_NORETURN inline void throw_bzip2_error(BZFILE* bzfile, const char* msg, int bzlib_error = 0) {
                std::string error("bzip2 error: ");
                error += msg;
                error += ": ";
                int errnum = bzlib_error;
                if (bzlib_error) {
                    error += std::to_string(bzlib_error);
                } else {
                    error += ::BZ2_bzerror(bzfile, &errnum);
                }
                throw osmium::bzip2_error(error, errnum);
            }

        } // namespace detail

        class Bzip2Compressor : public Compressor {

            FILE* m_file;
            int m_bzerror;
            BZFILE* m_bzfile;

        public:

            explicit Bzip2Compressor(int fd, fsync sync) :
                Compressor(sync),
                m_file(fdopen(dup(fd), "wb")),
                m_bzerror(BZ_OK),
                m_bzfile(::BZ2_bzWriteOpen(&m_bzerror, m_file, 6, 0, 0)) {
                if (!m_bzfile) {
                    detail::throw_bzip2_error(m_bzfile, "write open failed", m_bzerror);
                }
            }

            ~Bzip2Compressor() noexcept final {
                try {
                    close();
                } catch (...) {
                    // Ignore any exceptions because destructor must not throw.
                }
            }

            void write(const std::string& data) final {
                int error;
                ::BZ2_bzWrite(&error, m_bzfile, const_cast<char*>(data.data()), static_cast_with_assert<int>(data.size()));
                if (error != BZ_OK && error != BZ_STREAM_END) {
                    detail::throw_bzip2_error(m_bzfile, "write failed", error);
                }
            }

            void close() final {
                if (m_bzfile) {
                    int error;
                    ::BZ2_bzWriteClose(&error, m_bzfile, 0, nullptr, nullptr);
                    m_bzfile = nullptr;
                    if (m_file) {
                        if (do_fsync()) {
                            osmium::io::detail::reliable_fsync(::fileno(m_file));
                        }
                        if (fclose(m_file) != 0) {
                            throw std::system_error(errno, std::system_category(), "Close failed");
                        }
                    }
                    if (error != BZ_OK) {
                        detail::throw_bzip2_error(m_bzfile, "write close failed", error);
                    }
                }
            }

        }; // class Bzip2Compressor

        class Bzip2Decompressor : public Decompressor {

            FILE* m_file;
            int m_bzerror;
            BZFILE* m_bzfile;
            bool m_stream_end {false};

        public:

            explicit Bzip2Decompressor(int fd) :
                Decompressor(),
                m_file(fdopen(dup(fd), "rb")),
                m_bzerror(BZ_OK),
                m_bzfile(::BZ2_bzReadOpen(&m_bzerror, m_file, 0, 0, nullptr, 0)) {
                if (!m_bzfile) {
                    detail::throw_bzip2_error(m_bzfile, "read open failed", m_bzerror);
                }
            }

            ~Bzip2Decompressor() noexcept final {
                try {
                    close();
                } catch (...) {
                    // Ignore any exceptions because destructor must not throw.
                }
            }

            std::string read() final {
                std::string buffer;

                if (!m_stream_end) {
                    buffer.resize(osmium::io::Decompressor::input_buffer_size);
                    int error;
                    int nread = ::BZ2_bzRead(&error, m_bzfile, const_cast<char*>(buffer.data()), static_cast_with_assert<int>(buffer.size()));
                    if (error != BZ_OK && error != BZ_STREAM_END) {
                        detail::throw_bzip2_error(m_bzfile, "read failed", error);
                    }
                    if (error == BZ_STREAM_END) {
                        void* unused;
                        int nunused;
                        if (! feof(m_file)) {
                            ::BZ2_bzReadGetUnused(&error, m_bzfile, &unused, &nunused);
                            if (error != BZ_OK) {
                                detail::throw_bzip2_error(m_bzfile, "get unused failed", error);
                            }
                            std::string unused_data(static_cast<const char*>(unused), static_cast<std::string::size_type>(nunused));
                            ::BZ2_bzReadClose(&error, m_bzfile);
                            if (error != BZ_OK) {
                                detail::throw_bzip2_error(m_bzfile, "read close failed", error);
                            }
                            m_bzfile = ::BZ2_bzReadOpen(&error, m_file, 0, 0, const_cast<void*>(static_cast<const void*>(unused_data.data())), static_cast_with_assert<int>(unused_data.size()));
                            if (error != BZ_OK) {
                                detail::throw_bzip2_error(m_bzfile, "read open failed", error);
                            }
                        } else {
                            m_stream_end = true;
                        }
                    }
                    buffer.resize(static_cast<std::string::size_type>(nread));
                }

                return buffer;
            }

            void close() final {
                if (m_bzfile) {
                    int error;
                    ::BZ2_bzReadClose(&error, m_bzfile);
                    m_bzfile = nullptr;
                    if (m_file) {
                        if (fclose(m_file) != 0) {
                            throw std::system_error(errno, std::system_category(), "Close failed");
                        }
                    }
                    if (error != BZ_OK) {
                        detail::throw_bzip2_error(m_bzfile, "read close failed", error);
                    }
                }
            }

        }; // class Bzip2Decompressor

        class Bzip2BufferDecompressor : public Decompressor {

            const char* m_buffer;
            size_t m_buffer_size;
            bz_stream m_bzstream;

        public:

            Bzip2BufferDecompressor(const char* buffer, size_t size) :
                m_buffer(buffer),
                m_buffer_size(size),
                m_bzstream() {
                m_bzstream.next_in = const_cast<char*>(buffer);
                m_bzstream.avail_in = static_cast_with_assert<unsigned int>(size);
                int result = BZ2_bzDecompressInit(&m_bzstream, 0, 0);
                if (result != BZ_OK) {
                    std::string message("bzip2 error: decompression init failed: ");
                    throw bzip2_error(message, result);
                }
            }

            ~Bzip2BufferDecompressor() noexcept final {
                try {
                    close();
                } catch (...) {
                    // Ignore any exceptions because destructor must not throw.
                }
            }

            std::string read() final {
                std::string output;

                if (m_buffer) {
                    const size_t buffer_size = 10240;
                    output.resize(buffer_size);
                    m_bzstream.next_out = const_cast<char*>(output.data());
                    m_bzstream.avail_out = buffer_size;
                    int result = BZ2_bzDecompress(&m_bzstream);

                    if (result != BZ_OK) {
                        m_buffer = nullptr;
                        m_buffer_size = 0;
                    }

                    if (result != BZ_OK && result != BZ_STREAM_END) {
                        std::string message("bzip2 error: decompress failed: ");
                        throw bzip2_error(message, result);
                    }

                    output.resize(static_cast<unsigned long>(m_bzstream.next_out - output.data()));
                }

                return output;
            }

            void close() final {
                BZ2_bzDecompressEnd(&m_bzstream);
            }

        }; // class Bzip2BufferDecompressor

        namespace detail {

            // we want the register_compression() function to run, setting
            // the variable is only a side-effect, it will never be used
            const bool registered_bzip2_compression = osmium::io::CompressionFactory::instance().register_compression(osmium::io::file_compression::bzip2,
                [](int fd, fsync sync) { return new osmium::io::Bzip2Compressor(fd, sync); },
                [](int fd) { return new osmium::io::Bzip2Decompressor(fd); },
                [](const char* buffer, size_t size) { return new osmium::io::Bzip2BufferDecompressor(buffer, size); }
            );

            // dummy function to silence the unused variable warning from above
            inline bool get_registered_bzip2_compression() noexcept {
                return registered_bzip2_compression;
            }

        } // namespace detail

    } // namespace io

} // namespace osmium

#endif // OSMIUM_IO_BZIP2_COMPRESSION_HPP
