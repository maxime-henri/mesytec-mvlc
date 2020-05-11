#include "mvlc_listfile_zip.h"

#include <iostream>
#include <cassert>
#include <cstring>
#include <lz4frame.h>
#include <mz.h>
#include <mz_compat.h>
#include <mz_os.h>
#include <mz_strm.h>
#include <mz_strm_buf.h>
#include <mz_strm_os.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>
#include <sys/stat.h>

#include "util/storage_sizes.h"
#include "util/string_view.hpp"

using namespace nonstd;
using std::cout;
using std::endl;

namespace mesytec
{
namespace mvlc
{
namespace listfile
{

//
// ZipCreator
//

ZipEntryWriteHandle::ZipEntryWriteHandle(ZipCreator *creator)
    : m_zipCreator(creator)
{ }

ZipEntryWriteHandle::~ZipEntryWriteHandle()
{ }

size_t ZipEntryWriteHandle::write(const u8 *data, size_t size)
{
    return m_zipCreator->writeToCurrentEntry(data, size);
}

struct ZipCreator::Private
{
    struct LZ4WriteContext
    {
        struct CompressResult
        {
            int error;
            unsigned long long size_in;
            unsigned long long size_out;
        };

        static constexpr size_t ChunkSize = util::Megabytes(1);

        LZ4F_preferences_t lz4Prefs =
        {
            { LZ4F_max4MB, LZ4F_blockLinked, LZ4F_noContentChecksum, LZ4F_frame,
              0 /* unknown content size */, 0 /* no dictID */ , LZ4F_noBlockChecksum },
            0,   /* compression level; 0 == default */
            0,   /* autoflush */
            0,   /* favor decompression speed */
            { 0, 0, 0 },  /* reserved, must be set to 0 */
        };

        LZ4F_compressionContext_t ctx = {};
        std::vector<u8> buffer;

        void begin(int compressLevel)
        {
            if (auto err = LZ4F_createCompressionContext(&ctx, LZ4F_VERSION))
                throw std::runtime_error("LZ4F_createCompressionContext: " + std::to_string(err));

            lz4Prefs.compressionLevel = compressLevel;
            size_t bufferSize = LZ4F_compressBound(ChunkSize, &lz4Prefs);
            buffer = std::vector<u8>(bufferSize);
        }

        void end()
        {
            LZ4F_freeCompressionContext(ctx);   /* supports free on NULL */
        }
    };

    explicit Private(ZipCreator *q_)
        : entryWriteHandle(q_)
    {
        mz_stream_os_create(&mz_osStream);
        mz_stream_buffered_create(&mz_bufStream);
        mz_stream_set_base(mz_bufStream, mz_osStream);

        mz_zip_writer_create(&mz_zipWriter);
        mz_zip_writer_set_follow_links(mz_zipWriter, true);
    }

    ~Private()
    {
        mz_zip_writer_delete(&mz_zipWriter);
        mz_stream_delete(&mz_bufStream);
        mz_stream_delete(&mz_osStream);

        mz_zipWriter = nullptr;
        mz_bufStream = nullptr;
        mz_osStream = nullptr;
    }

    size_t writeToCurrentZIPEntry(const u8 *data, size_t size)
    {
        s32 bytesWritten = mz_zip_writer_entry_write(mz_zipWriter, data, size);

        if (bytesWritten < 0)
            throw std::runtime_error("mz_zip_writer_entry_write: " + std::to_string(bytesWritten));

        assert(static_cast<size_t>(bytesWritten) == size);

        return static_cast<size_t>(bytesWritten);
    }

    void *mz_zipWriter = nullptr;
    void *mz_bufStream = nullptr;
    void *mz_osStream = nullptr;

    ZipEntryInfo entryInfo;

    LZ4WriteContext lz4Ctx;

    ZipEntryWriteHandle entryWriteHandle;
};

ZipCreator::ZipCreator()
    : d(std::make_unique<Private>(this))
{}

ZipCreator::~ZipCreator()
{
    try
    {
        closeCurrentEntry();
    }
    catch (...)
    {}

    try
    {
        closeArchive();
    }
    catch (...)
    {}

}

void ZipCreator::createArchive(const std::string &zipFilename)
{
    s32 mzMode = MZ_OPEN_MODE_WRITE | MZ_OPEN_MODE_CREATE;

    if (auto err = mz_stream_open(d->mz_bufStream, zipFilename.c_str(), mzMode))
        throw std::runtime_error("mz_stream_open: " + std::to_string(err));

    if (auto err = mz_zip_writer_open(d->mz_zipWriter, d->mz_bufStream))
        throw std::runtime_error("mz_zip_writer_open: " + std::to_string(err));
}

void ZipCreator::closeArchive()
{
    if (auto err = mz_zip_writer_close(d->mz_zipWriter))
        throw std::runtime_error("mz_zip_writer_close: " + std::to_string(err));

    if (auto err = mz_stream_close(d->mz_bufStream))
        throw std::runtime_error("mz_stream_close: " + std::to_string(err));
}

bool ZipCreator::isOpen() const
{
    return mz_stream_is_open(d->mz_bufStream) == MZ_OK;
}

ZipEntryWriteHandle *ZipCreator::createZIPEntry(const std::string &entryName, int compressLevel)
{
    if (hasOpenEntry())
        throw std::runtime_error("ZipCreator has open archive entry");

    mz_zip_file file_info = {};
    file_info.filename = entryName.c_str();
    file_info.modified_date = time(nullptr);
    file_info.version_madeby = MZ_VERSION_MADEBY;
    file_info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
    file_info.zip64 = MZ_ZIP64_FORCE;
    file_info.external_fa = (S_IFREG) | (0644u << 16);

    mz_zip_writer_set_compress_method(d->mz_zipWriter, MZ_COMPRESS_METHOD_DEFLATE);
    mz_zip_writer_set_compress_level(d->mz_zipWriter, compressLevel);

    if (auto err = mz_zip_writer_entry_open(d->mz_zipWriter, &file_info))
        throw std::runtime_error("mz_zip_writer_entry_open: " + std::to_string(err));

    d->entryInfo = {};
    d->entryInfo.type = ZipEntryInfo::ZIP;
    d->entryInfo.name = entryName;
    d->entryInfo.isOpen = true;

    return &d->entryWriteHandle;
}

ZipEntryWriteHandle *ZipCreator::createLZ4Entry(const std::string &entryName_, int compressLevel)
{
    if (hasOpenEntry())
        throw std::runtime_error("ZipCreator has open archive entry");

    const std::string entryName = entryName_ + ".lz4";

    mz_zip_file file_info = {};
    file_info.filename = entryName.c_str();
    file_info.modified_date = time(nullptr);
    file_info.version_madeby = MZ_VERSION_MADEBY;
    file_info.compression_method = MZ_COMPRESS_METHOD_STORE;
    file_info.zip64 = MZ_ZIP64_FORCE;
    file_info.external_fa = (S_IFREG) | (0644u << 16);

    mz_zip_writer_set_compress_method(d->mz_zipWriter, MZ_COMPRESS_METHOD_STORE);
    mz_zip_writer_set_compress_level(d->mz_zipWriter, 0);

    if (auto err = mz_zip_writer_entry_open(d->mz_zipWriter, &file_info))
        throw std::runtime_error("mz_zip_writer_entry_open: " + std::to_string(err));

    d->entryInfo = {};
    d->entryInfo.type = ZipEntryInfo::LZ4;
    d->entryInfo.name = entryName;
    d->entryInfo.isOpen = true;

    d->lz4Ctx.begin(compressLevel);

    // write the LZ4 frame header
    size_t lz4BufferBytes = LZ4F_compressBegin(
        d->lz4Ctx.ctx,
        d->lz4Ctx.buffer.data(),
        d->lz4Ctx.buffer.size(),
        &d->lz4Ctx.lz4Prefs);

    if (LZ4F_isError(lz4BufferBytes))
        throw std::runtime_error("LZ4F_compressBegin: " + std::to_string(lz4BufferBytes));

    // flush the LZ4 buffer contents to the ZIP
    d->writeToCurrentZIPEntry(d->lz4Ctx.buffer.data(), lz4BufferBytes);

    d->entryInfo.bytesWritten += lz4BufferBytes;
    d->entryInfo.lz4CompressedBytesWritten += lz4BufferBytes;

    return &d->entryWriteHandle;
}

bool ZipCreator::hasOpenEntry() const
{
    return d->entryInfo.isOpen;
}

const ZipEntryInfo &ZipCreator::entryInfo() const
{
    return d->entryInfo;
}

size_t ZipCreator::writeToCurrentEntry(const u8 *inputData, size_t inputSize)
{
    if (!hasOpenEntry())
        throw std::runtime_error("ZipCreator has no open archive entry");

    size_t bytesWritten = 0u;

    switch (d->entryInfo.type)
    {
        case ZipEntryInfo::ZIP:
            bytesWritten = d->writeToCurrentZIPEntry(inputData, inputSize);
            break;

        case ZipEntryInfo::LZ4:
            while (bytesWritten < inputSize)
            {
                size_t bytesLeft = inputSize - bytesWritten;
                size_t chunkBytes = std::min(bytesLeft, d->lz4Ctx.buffer.size());

                assert(inputData + bytesWritten + chunkBytes <= inputData + inputSize);
                assert(chunkBytes <= LZ4F_compressBound(d->lz4Ctx.ChunkSize, &d->lz4Ctx.lz4Prefs));

                // compress the chunk into the LZ4WriteContext buffer
                size_t compressedSize = LZ4F_compressUpdate(
                    d->lz4Ctx.ctx,
                    d->lz4Ctx.buffer.data(), d->lz4Ctx.buffer.size(),
                    inputData + bytesWritten, chunkBytes,
                    nullptr);

                if (LZ4F_isError(compressedSize))
                    throw std::runtime_error("LZ4F_compressUpdate: " + std::to_string(compressedSize));

                // flush the LZ4 buffer contents to the ZIP
                d->writeToCurrentZIPEntry(d->lz4Ctx.buffer.data(), compressedSize);

                bytesWritten += chunkBytes;
                d->entryInfo.bytesWritten += chunkBytes;
                d->entryInfo.lz4CompressedBytesWritten += compressedSize;
            }
            break;
    };

    return bytesWritten;
}

void ZipCreator::closeCurrentEntry()
{
    if (!hasOpenEntry())
        throw std::runtime_error("ZipCreator has no open archive entry");

    if (d->entryInfo.type == ZipEntryInfo::LZ4)
    {
        // flush whatever remains within internal buffers
        size_t const compressedSize = LZ4F_compressEnd(
            d->lz4Ctx.ctx,
            d->lz4Ctx.buffer.data(), d->lz4Ctx.buffer.size(),
            nullptr);

        if (LZ4F_isError(compressedSize))
            throw std::runtime_error("LZ4F_compressEnd: " + std::to_string(compressedSize));

        // flush the LZ4 buffer contents to the ZIP
        size_t bytesWritten = d->writeToCurrentZIPEntry(d->lz4Ctx.buffer.data(), compressedSize);

        d->entryInfo.bytesWritten += bytesWritten;
        d->entryInfo.lz4CompressedBytesWritten += compressedSize;
    }

    if (auto err = mz_zip_writer_entry_close(d->mz_zipWriter))
        throw std::runtime_error("mz_zip_writer_entry_close: " + std::to_string(err));

    d->entryInfo.isOpen = false;
}

//
// ZipReader
//

size_t ZipReadHandle::read(u8 *dest, size_t maxSize)
{
    return m_zipReader->readCurrentEntry(dest, maxSize);
}

void ZipReadHandle::seek(size_t pos)
{
    std::string currentName = m_zipReader->currentEntryName();
    m_zipReader->closeCurrentEntry();
    m_zipReader->openEntry(currentName);

    std::vector<u8> buffer(util::Megabytes(1));

    while (pos > 0)
    {
        pos -= read(buffer.data(), std::min(buffer.size(), pos));
    }
}


struct ZipReader::Private
{
    struct LZ4ReadContext
    {
        static constexpr size_t ChunkSize = util::Megabytes(1);

        LZ4F_decompressionContext_t ctx = {};
        std::vector<u8> compressedBuffer;
        std::vector<u8> decompressedBuffer;
        basic_string_view<u8> compressedView;
        basic_string_view<u8> decompressedView;

        LZ4ReadContext()
            : compressedBuffer(ChunkSize)
        {
            if (auto err = LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION))
                throw std::runtime_error("LZ4F_createDecompressionContext: " + std::to_string(err));
        }

        ~LZ4ReadContext()
        {
            LZ4F_freeDecompressionContext(ctx);
        }

        void clear()
        {
            LZ4F_resetDecompressionContext(ctx);
            compressedBuffer.clear();
            compressedBuffer.resize(ChunkSize);
            decompressedBuffer.clear();
            compressedView = {};
            decompressedView = {};
        }
    };

    explicit Private(ZipReader *q_)
        : entryReadHandle(q_)
    {
    }

    ~Private()
    {
    }

    size_t readFromCurrentZipEntry(u8 *dest, size_t maxSize)
    {
        s32 res = mz_zip_reader_entry_read(this->reader, dest, maxSize);

        //cout << __PRETTY_FUNCTION__ << " maxSize=" << maxSize
        //    << ", res=" << res << endl;

        if (res < 0)
            throw std::runtime_error("mz_zip_reader_entry_read: " + std::to_string(res));

        return static_cast<size_t>(res);
    }

    void *reader = nullptr;
    void *osStream = nullptr;
    std::vector<std::string> entryNameCache;
    // IMPORTANT: The variable sized fields filename, extrafield, comment and
    // linkname are set to nullptr when stored in the vector.
    std::vector<mz_zip_entry> entryInfoCache;
    ZipReadHandle entryReadHandle = nullptr;
    ZipEntryInfo entryInfo;
    LZ4ReadContext lz4Ctx;
};

ZipReader::ZipReader()
    : d(std::make_unique<Private>(this))
{
    mz_zip_reader_create(&d->reader);
    mz_stream_os_create(&d->osStream);
}

ZipReader::~ZipReader()
{
    mz_zip_reader_delete(&d->reader);
    mz_stream_os_delete(&d->osStream);
}

void ZipReader::openArchive(const std::string &archiveName)
{
    if (auto err = mz_stream_os_open(d->osStream, archiveName.c_str(), MZ_OPEN_MODE_READ))
        throw std::runtime_error("mz_stream_os_open: " + std::to_string(err));

    if (auto err = mz_zip_reader_open(d->reader, d->osStream))
        throw std::runtime_error("mz_zip_reader_open: " + std::to_string(err));

    if (auto err = mz_zip_reader_goto_first_entry(d->reader))
    {
        if (err != MZ_END_OF_LIST)
            throw std::runtime_error("mz_zip_reader_goto_first_entry: " + std::to_string(err));
    }

    d->entryNameCache = {};
    s32 err = MZ_OK;

    do
    {
        mz_zip_file *entryInfo = nullptr;
        if ((err = mz_zip_reader_entry_get_info(d->reader, &entryInfo)))
            throw std::runtime_error("mz_zip_reader_entry_get_info: " + std::to_string(err));

        d->entryNameCache.push_back(entryInfo->filename);

        mz_zip_entry entryCopy = *entryInfo;
        entryCopy.filename = nullptr;
        entryCopy.extrafield = nullptr;
        entryCopy.comment = nullptr;
        entryCopy.linkname = nullptr;
        d->entryInfoCache.push_back(entryCopy);
    } while ((err = mz_zip_reader_goto_next_entry(d->reader)) == MZ_OK);

    if (err != MZ_END_OF_LIST)
        throw std::runtime_error("mz_zip_reader_goto_next_entry: " + std::to_string(err));
}

void ZipReader::closeArchive()
{
    if (auto err = mz_zip_reader_close(d->reader))
        throw std::runtime_error("mz_zip_reader_close: " + std::to_string(err));

    if (auto err = mz_stream_os_close(d->osStream))
        throw std::runtime_error("mz_stream_os_close: " + std::to_string(err));

    d->entryNameCache = {};
}

std::vector<std::string> ZipReader::entryNameList()
{
    return d->entryNameCache;
}

namespace
{

inline size_t get_block_size(const LZ4F_frameInfo_t* info)
{
    switch (info->blockSizeID)
    {
        case LZ4F_default:
        case LZ4F_max64KB:  return 1 << 16;
        case LZ4F_max256KB: return 1 << 18;
        case LZ4F_max1MB:   return 1 << 20;
        case LZ4F_max4MB:   return 1 << 22;
        default:
                            throw std::runtime_error(
                                "impossible LZ4 block size with current frame specification (<=v1.6.1)");
    }
}

} // end anon namespace

ZipReadHandle *ZipReader::openEntry(const std::string &name)
{
    if (auto err = mz_zip_reader_locate_entry(d->reader, name.c_str(), false))
        throw std::runtime_error("mz_zip_reader_locate_entry: " + std::to_string(err));

    if (auto err = mz_zip_reader_entry_open(d->reader))
        throw std::runtime_error("mz_zip_reader_entry_open: " + std::to_string(err));

    mz_zip_file *mzEntryInfo = nullptr;

    if (auto err = mz_zip_reader_entry_get_info(d->reader, &mzEntryInfo))
        throw std::runtime_error("mz_zip_reader_entry_get_info: " + std::to_string(err));

    d->lz4Ctx.clear();
    d->entryInfo = {};
    d->entryInfo.name = name;
    d->entryInfo.compressedSize = mzEntryInfo->compressed_size;
    d->entryInfo.uncompressedSize = mzEntryInfo->uncompressed_size;

    if (name.size() >= 4)
    {
        auto suffix = string_view(name.data() + (name.length() - 4), 4);

        if (suffix == ".lz4")
        {
            d->entryInfo.type = ZipEntryInfo::LZ4;
        }
    }

    if (d->entryInfo.type == ZipEntryInfo::LZ4)
    {
        size_t bytesRead = d->readFromCurrentZipEntry(
            d->lz4Ctx.compressedBuffer.data(),
            d->lz4Ctx.compressedBuffer.size());

        //cout << __PRETTY_FUNCTION__ << ": initial read from zip yielded " << bytesRead << " bytes" << endl;

        d->entryInfo.lz4CompressedBytesRead += bytesRead;

        if (bytesRead == 0)
            throw std::runtime_error("ZipReader::openEntry: not enough data to initialise LZ4 decompression");

        d->lz4Ctx.compressedView = { d->lz4Ctx.compressedBuffer.data(), bytesRead };

        assert(d->lz4Ctx.compressedView.size() == bytesRead);

        LZ4F_frameInfo_t info = {};

        size_t bytesConsumed = bytesRead;

        size_t res = LZ4F_getFrameInfo(
            d->lz4Ctx.ctx, &info, d->lz4Ctx.compressedView.data(), &bytesConsumed);

        if (LZ4F_isError(res))
            throw std::runtime_error("LZ4F_getFrameInfo: " + std::to_string(res));

        assert(d->lz4Ctx.compressedView.size() >= bytesConsumed);

        //cout << __PRETTY_FUNCTION__ << " LZ4F_getFrameInfo consumed " << bytesConsumed << " bytes" << endl;
        //for (size_t i=0; i<bytesConsumed; ++i)
        //    cout << __PRETTY_FUNCTION__ << "   " << i << ": "
        //      << std::hex << static_cast<unsigned>(d->lz4Ctx.compressedView[i]) << std::dec << endl;

        d->lz4Ctx.compressedView.remove_prefix(bytesConsumed);

        size_t dstCapacity = get_block_size(&info);

        //cout << __PRETTY_FUNCTION__ << " dstCapacity from LZ4 frameInfo: " << dstCapacity << endl;
        //cout << __PRETTY_FUNCTION__ << " compressedView.size() for first read is " << d->lz4Ctx.compressedView.size() << endl;

        d->lz4Ctx.decompressedBuffer.resize(dstCapacity);
    }

    d->entryInfo.isOpen = true;

    return &d->entryReadHandle;
}

ZipReadHandle *ZipReader::currentEntry()
{
    return &d->entryReadHandle;
}

void ZipReader::closeCurrentEntry()
{
    auto err = mz_zip_reader_entry_close(d->reader);

    if (err != MZ_OK && err != MZ_CRC_ERROR)
        throw std::runtime_error("mz_zip_reader_entry_close: " + std::to_string(err));
}

size_t ZipReader::readCurrentEntry(u8 *dest, size_t maxSize)
{
    if (d->entryInfo.type == ZipEntryInfo::ZIP)
        return d->readFromCurrentZipEntry(dest, maxSize);

    assert(d->entryInfo.type == ZipEntryInfo::LZ4);

    size_t retval = 0u;
    size_t loop = 0u;

    while (maxSize - retval > 0)
    {
        if (d->lz4Ctx.decompressedView.empty())
        {
            //cout << __PRETTY_FUNCTION__ << " loop #" << loop << ": decompressedView is empty, decompressing more data" << endl;
            //cout << __PRETTY_FUNCTION__ << " loop #" << loop << ": compressedView.size()=" << d->lz4Ctx.compressedView.size() << endl;

            if (d->lz4Ctx.compressedView.empty())
            {
                //cout << __PRETTY_FUNCTION__ << " loop #" << loop << ": compressedView is empty, reading more data from zip" << endl;

                size_t bytesRead = d->readFromCurrentZipEntry(
                    d->lz4Ctx.compressedBuffer.data(), d->lz4Ctx.compressedBuffer.size());

                d->lz4Ctx.compressedView = { d->lz4Ctx.compressedBuffer.data(), bytesRead };

                //cout << __PRETTY_FUNCTION__ << "read " << bytesRead << " bytes of uncompressed data" << endl;

                if (bytesRead == 0)
                    break;
            }

            assert(!d->lz4Ctx.compressedView.empty());

            // decompress from compressedBuffer into decompressedBuffer

            size_t decompressedSize = d->lz4Ctx.decompressedBuffer.size();
            size_t compressedSize = d->lz4Ctx.compressedView.size();

            s32 res = LZ4F_decompress(
                d->lz4Ctx.ctx,
                d->lz4Ctx.decompressedBuffer.data(), &decompressedSize, // dest
                d->lz4Ctx.compressedView.data(), &compressedSize,  // source
                nullptr); // options

            if (res == 0)
                cout << __PRETTY_FUNCTION__ << " loop #" << loop << ": LZ4F_decompress returned " << res << endl;

            //cout << __PRETTY_FUNCTION__ << " LZ4F_decompress: "
            //    << ", compressedSize=" << compressedSize
            //    << ", decompressedSize=" << decompressedSize
            //    << ", res=" << res
            //    << endl;

            if (LZ4F_isError(res))
                throw std::runtime_error("LZ4F_decompress: " + std::to_string(res));

            d->lz4Ctx.decompressedView = { d->lz4Ctx.decompressedBuffer.data(), decompressedSize };
            d->lz4Ctx.compressedView.remove_prefix(compressedSize);
        }

        size_t toCopy = std::min(d->lz4Ctx.decompressedView.size(), maxSize);
        std::memcpy(dest+retval, d->lz4Ctx.decompressedView.data(), toCopy);
        d->lz4Ctx.decompressedView.remove_prefix(toCopy);
        retval += toCopy;

        /*
        if (toCopy)
        {
            std::cout << __PRETTY_FUNCTION__ << " loop #" << loop
                << ": copied " << toCopy << " bytes from decompressed view into dest buffer"
                << ", dest[0]=" << std::hex << static_cast<unsigned>(dest[0])
                << ", dest[1]=" << std::hex << static_cast<unsigned>(dest[1])
                << ", dest[2]=" << std::hex << static_cast<unsigned>(dest[2])
                << ", dest[3]=" << std::hex << static_cast<unsigned>(dest[3])
                << ", dest[4]=" << std::hex << static_cast<unsigned>(dest[4])
                << std::dec
                << endl;
        }
        */

        ++loop;
    }

    //cout << __PRETTY_FUNCTION__ << "read of maxSize=" << maxSize
    //    << " satisfied after " << loop << " loops" << endl;

    return retval;
}

std::string ZipReader::currentEntryName() const
{
    return d->entryInfo.name;
}

const ZipEntryInfo &ZipReader::entryInfo() const
{
    return d->entryInfo;
}

} // end namespace listfile
} // end namespace mvlc
} // end namespace mesytec
