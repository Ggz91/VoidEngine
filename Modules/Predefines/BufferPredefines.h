#pragma once

namespace BufferPredefines
{
	const unsigned int UploadChunkSize = 256;
	const unsigned int MaxCommandAllocNum = 5;
	const unsigned int HiZBufferMinSize = 4;

	const unsigned int MaxMatNum = 10;
	const unsigned int MaxTextureNum = 10;
}

#define UploadBufferChunkSize BufferPredefines::UploadChunkSize
#define MaxCommandAllocNum BufferPredefines::MaxCommandAllocNum
#define HiZBufferMinSize BufferPredefines::HiZBufferMinSize
#define MaxMatNum BufferPredefines::MaxMatNum
#define MaxTextureNum BufferPredefines::MaxTextureNum
