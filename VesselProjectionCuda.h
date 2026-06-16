#ifndef VESSELPROJECTIONCUDA_H
#define VESSELPROJECTIONCUDA_H

#include <cstdint>
#include <string>

struct VesselCudaProcessor;

bool createVesselCudaProcessor(const float *spectralWindow,
                               int ascanLen,
                               int bscanLen,
                               int angioRep,
                               int cropDepth,
                               VesselCudaProcessor **processor,
                               std::string *deviceDescription,
                               std::string *errorMessage);

void destroyVesselCudaProcessor(VesselCudaProcessor *processor);

bool computeFftRowsCuda(VesselCudaProcessor *processor,
                        const uint16_t *rawBlock,
                        float *complexOutputInterleaved,
                        std::string *errorMessage);

bool computeStructuralAndFlowCuda(VesselCudaProcessor *processor,
                                  const float *registeredFramesInterleaved,
                                  float *structuralBlock,
                                  float *flowBlock,
                                  std::string *errorMessage);

bool computeRegisteredStructuralAndFlowCuda(VesselCudaProcessor *processor,
                                            const uint16_t *rawBlock,
                                            int cropStart0,
                                            int maxRegistrationShiftPixels,
                                            float *structuralBlock,
                                            float *flowBlock,
                                            std::string *errorMessage);

#endif // VESSELPROJECTIONCUDA_H
