/**
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */
#include "BookieCodecV2.h"

#include "Logging.h"

DECLARE_LOG_OBJECT();

struct PacketHeader {
    int8_t version;
    BookieOperation opCode;
    int16_t flags;

    static PacketHeader fromInt(int value) {
        PacketHeader hdr;
        hdr.version = value >> 24;
        hdr.opCode = (BookieOperation) ((value >> 16) & 0xFF);
        hdr.flags = value & 0xFF;
        return hdr;
    }

    int toInt() const {
        return ((version & 0xFF) << 24) | (((int8_t) opCode & 0xFF) << 16) | ((int16_t) flags & 0xFFFF);
    }
};

void BookieServerCodecV2::read(Context* ctx, IOBufPtr buf) {
    if (!buf) {
        return;
    }

    io::Cursor reader { buf.get() };
    if (reader.totalLength() < sizeof(int32_t)) {
        // Short request
        ctx->fireClose();
        return;
    }

    Request request;
    PacketHeader hdr = PacketHeader::fromInt(reader.readBE<int32_t>());
    request.protocolVersion = hdr.version;
    request.opCode = hdr.opCode;
    request.flags = hdr.flags;

    switch (request.opCode) {
    case BookieOperation::AddEntry:
        static const int32_t addRequestSize = BookieConstant::MasterKeyLength + 2 * sizeof(int64_t);
        if (reader.totalLength() < addRequestSize) {
            LOG_WARN(
                    "Invalid add entry request size: " << reader.totalLength() << " -- expecting at least: " << addRequestSize);
            ctx->fireClose();
            return;
        }
        reader.skip(BookieConstant::MasterKeyLength);
        request.ledgerId = reader.readBE<int64_t>();
        request.entryId = reader.readBE<int64_t>();

        reader.clone(request.data, reader.totalLength());
        break;

    case BookieOperation::ReadEntry: {
        const int32_t readRequestSize = 2 * sizeof(int64_t)
                + (request.isFencing() ? BookieConstant::MasterKeyLength : 0);
        if (reader.totalLength() < readRequestSize) {
            LOG_WARN(
                    "Invalid read entry request size: " << reader.totalLength() << " -- expecting: " << readRequestSize);
            ctx->fireClose();
            return;
        }

        request.ledgerId = reader.readBE<int64_t>();
        request.entryId = reader.readBE<int64_t>();

        if (request.isFencing()) {
            // Fencing reads will provide the master key which we'll ignore
            reader.skip(BookieConstant::MasterKeyLength);
        }
        break;
    }
    case BookieOperation::Auth:
        break;
    }

    LOG_DEBUG("Deserialized request: " << request);
    ctx->fireRead(std::move(request));
}

Future<Unit> BookieServerCodecV2::write(Context* ctx, Response response) {
    LOG_DEBUG("Serializing response: " << response);

    const int headerSize = 4 + 24;
    const int frameSize = headerSize /* + sizeof(data) */;
    const int bufferSize = frameSize + 4;
    IOBufPtr buf = IOBuf::create(bufferSize);
    buf->append(bufferSize);

    PacketHeader pktHeader { response.protocolVersion, response.opCode, 0 };
    io::RWPrivateCursor writer(buf.get());

    writer.writeBE<int32_t>(frameSize);
    writer.writeBE<int32_t>(pktHeader.toInt());

    switch (response.opCode) {
    case BookieOperation::AddEntry:
        writer.writeBE<int32_t>((int32_t) response.errorCode);
        writer.writeBE<int64_t>(response.ledgerId);
        writer.writeBE<int64_t>(response.entryId);
        break;

    case BookieOperation::ReadEntry:
        writer.writeBE<int32_t>((int32_t) response.errorCode);
        writer.writeBE<int64_t>(response.ledgerId);
        writer.writeBE<int64_t>(response.entryId);

        if (response.data) {
            // Also write data
//            buf->
        }

        break;

    case BookieOperation::Auth:
        break;
    }

    return ctx->fireWrite(std::move(buf));
}

void BookieClientCodecV2::read(Context* ctx, IOBufPtr buf) {
    if (!buf) {
        return;
    }

    io::Cursor reader { buf.get() };

    LOG_DEBUG("Received response: len=" << reader.totalLength());
    if (reader.totalLength() < sizeof(int32_t)) {
        // Short request
        ctx->fireClose();
        return;
    }

    Response response;
    PacketHeader hdr = PacketHeader::fromInt(reader.readBE<int32_t>());
    response.protocolVersion = hdr.version;
    response.opCode = hdr.opCode;

    switch (response.opCode) {
    case BookieOperation::AddEntry:
        response.errorCode = (BookieError) reader.readBE<int32_t>();
        response.ledgerId = reader.readBE<int64_t>();
        response.entryId = reader.readBE<int64_t>();
        break;

    case BookieOperation::ReadEntry: {
        response.errorCode = (BookieError) reader.readBE<int32_t>();
        response.ledgerId = reader.readBE<int64_t>();
        response.entryId = reader.readBE<int64_t>();
        // TODO
        break;
    }
    case BookieOperation::Auth:
        // TODO
        break;
    }

    LOG_DEBUG("Deserialized response: " << response);
    ctx->fireRead(std::move(response));
}

Future<Unit> BookieClientCodecV2::write(Context* ctx, Request request) {
    LOG_DEBUG("Serializing request: " << request);

    constexpr int headerSize = sizeof(int32_t) + BookieConstant::MasterKeyLength + 2 * sizeof(int64_t);
    const int frameSize = headerSize + request.data->length();
    const int bufferSize = headerSize + 4;

    IOBufPtr buffer = IOBuf::create(bufferSize);
    buffer->append(bufferSize);

    PacketHeader pktHeader { request.protocolVersion, request.opCode, request.flags };
    io::RWPrivateCursor writer(buffer.get());

    writer.writeBE<int32_t>(frameSize);
    writer.writeBE<int32_t>(pktHeader.toInt());

    switch (request.opCode) {
    case BookieOperation::AddEntry:
        writer.skip(BookieConstant::MasterKeyLength);
        writer.writeBE<int64_t>(request.ledgerId);
        writer.writeBE<int64_t>(request.entryId);
        writer.insert(std::move(request.data));
        break;

    case BookieOperation::ReadEntry:
        // TODO
        break;

    case BookieOperation::Auth:
        // TODO
        break;
    }

    return ctx->fireWrite(std::move(buffer));
}

