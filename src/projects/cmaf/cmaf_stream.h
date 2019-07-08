//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Jaejong Bong
//  Copyright (c) 2019 AirenSoft. All rights reserved.
//
//==============================================================================

#pragma once
#include "segment_stream/segment_stream.h"
#include "cmaf_stream_packetyzer.h"

//====================================================================================================
// CmafStream
//====================================================================================================
class CmafStream : public SegmentStream
{
public:
    static std::shared_ptr<CmafStream> Create(int segment_count,
                                               int segment_duration,
                                               const std::shared_ptr<Application> application,
                                               const StreamInfo &info,
                                               uint32_t worker_count,
                                               const std::shared_ptr<ICmafChunkedTransfer> &chunked_transfer);

    CmafStream(const std::shared_ptr<Application> application,
    		const StreamInfo &info,
    		const std::shared_ptr<ICmafChunkedTransfer> &chunked_transfer);

	~CmafStream();

    std::shared_ptr<StreamPacketyzer> CreateStreamPacketyzer(int segment_count,
												int segment_duration,
												const  ov::String &segment_prefix,
												PacketyzerStreamType stream_type,
												PacketyzerMediaInfo media_info) override
    {
        auto stream_packetyzer = std::make_shared<CmafStreamPacketyzer>(GetApplication()->GetName(),
                                                                        GetName(),
                                                                        segment_count,
                                                                        segment_duration,
                                                                        segment_prefix,
                                                                        stream_type,
                                                                        media_info,
																		_chunked_transfer);

        return std::static_pointer_cast<StreamPacketyzer>(stream_packetyzer);
    }

private:
	std::shared_ptr<ICmafChunkedTransfer> _chunked_transfer = nullptr;
};
