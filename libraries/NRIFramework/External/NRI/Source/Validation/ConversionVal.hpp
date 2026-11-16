// © 2021 NVIDIA Corporation

void nri::ConvertBotomLevelGeometries(const BottomLevelGeometryDesc* geometries, uint32_t geometryNum, BottomLevelGeometryDesc*& outGeometries, BottomLevelMicromapDesc*& outMicromaps) {
    // AccelerationStructureDesc permits HAS_BUFFER during prebuild queries.
    // It denotes presence, not a validation wrapper that may be dereferenced.
    const auto bufferImpl = [](const Buffer* buffer) -> const Buffer* {
        return buffer == HAS_BUFFER ? buffer : NRI_GET_IMPL(Buffer, buffer);
    };
    for (uint32_t i = 0; i < geometryNum; i++) {
        const BottomLevelGeometryDesc& src = geometries[i];

        BottomLevelGeometryDesc& dst = *outGeometries++;
        dst = src;

        if (src.type == BottomLevelGeometryType::TRIANGLES) {
            dst.triangles.vertexBuffer = bufferImpl(src.triangles.vertexBuffer);
            dst.triangles.indexBuffer = bufferImpl(src.triangles.indexBuffer);
            dst.triangles.transformBuffer = bufferImpl(src.triangles.transformBuffer);

            if (src.triangles.micromap) {
                dst.triangles.micromap = outMicromaps++;

                *dst.triangles.micromap = *src.triangles.micromap;
                dst.triangles.micromap->micromap = NRI_GET_IMPL(Micromap, src.triangles.micromap->micromap);
                dst.triangles.micromap->indexBuffer = bufferImpl(src.triangles.micromap->indexBuffer);
            }
        } else
            dst.aabbs.buffer = bufferImpl(src.aabbs.buffer);
    }
}
