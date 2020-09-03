#pragma once

namespace BufferPredefines
{
	const unsigned int UploadChunkSize = 256;
	const unsigned int MaxCommandAllocNum = 5;
}

#define UploadBufferChunkSize BufferPredefines::UploadChunkSize
#define MaxCommandAllocNum BufferPredefines::MaxCommandAllocNum
