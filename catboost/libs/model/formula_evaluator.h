#pragma once

#include "model.h"
#include <catboost/libs/helpers/exception.h>
#include <emmintrin.h>

constexpr size_t FORMULA_EVALUATION_BLOCK_SIZE = 128;

inline void OneHotBinsFromTransposedCatFeatures(
    const TVector<TOneHotFeature>& OneHotFeatures,
    const THashMap<int, int> catFeaturePackedIndex,
    const size_t docCount,
    ui8*& result,
    TVector<int>& transposedHash) {
    for (const auto& oheFeature : OneHotFeatures) {
        const auto catIdx = catFeaturePackedIndex.at(oheFeature.CatFeatureIndex);
        for (size_t docId = 0; docId < docCount; ++docId) {
            const auto val = transposedHash[catIdx * docCount + docId];
            for (size_t borderIdx = 0; borderIdx < oheFeature.Values.size(); ++borderIdx) {
                result[docId] |= (ui8)(val == oheFeature.Values[borderIdx]) * (borderIdx + 1);
            }
        }
        result += docCount;
    }
}

#ifdef NO_SSE
template<typename TFloatFeatureAccessor>
Y_FORCE_INLINE void BinarizeFloats(const size_t docCount, TFloatFeatureAccessor floatAccessor, const TConstArrayRef<float> borders, size_t start, ui8*& result) {
    const auto docCount8 = (docCount | 0x7) ^ 0x7;
    for (size_t docId = 0; docId < docCount8; docId += 8) {
        const float val[8] = {
            floatAccessor(start + docId + 0),
            floatAccessor(start + docId + 1),
            floatAccessor(start + docId + 2),
            floatAccessor(start + docId + 3),
            floatAccessor(start + docId + 4),
            floatAccessor(start + docId + 5),
            floatAccessor(start + docId + 6),
            floatAccessor(start + docId + 7)
        };
        auto writePtr = (ui32*)(result + docId);
        for (const auto border : borders) {
            writePtr[0] += (val[0] > border) + ((val[1] > border) << 8) + ((val[2] > border) << 16) + ((val[3] > border) << 24);
            writePtr[1] += (val[4] > border) + ((val[5] > border) << 8) + ((val[6] > border) << 16) + ((val[7] > border) << 24);
        }
    }
    for (size_t docId = docCount8; docId < docCount; ++docId) {
        const auto val = floatAccessor(start + docId);
        for (const auto border : borders) {
            result[docId] += (ui8)(val > border);
        }
    }
    result += docCount;
}

#else

template<typename TFloatFeatureAccessor>
Y_FORCE_INLINE void BinarizeFloats(const size_t docCount, TFloatFeatureAccessor floatAccessor, const TConstArrayRef<float> borders, size_t start, ui8*& result) {
    const auto docCount16 = (docCount | 0xf) ^ 0xf;
    for (size_t docId = 0; docId < docCount16; docId += 16) {
        const float val[16] = {
            floatAccessor(start + docId + 0),
            floatAccessor(start + docId + 1),
            floatAccessor(start + docId + 2),
            floatAccessor(start + docId + 3),
            floatAccessor(start + docId + 4),
            floatAccessor(start + docId + 5),
            floatAccessor(start + docId + 6),
            floatAccessor(start + docId + 7),
            floatAccessor(start + docId + 8),
            floatAccessor(start + docId + 9),
            floatAccessor(start + docId + 10),
            floatAccessor(start + docId + 11),
            floatAccessor(start + docId + 12),
            floatAccessor(start + docId + 13),
            floatAccessor(start + docId + 14),
            floatAccessor(start + docId + 15)
        };
        const __m128i mask = _mm_set1_epi8(1);
        const __m128 floats0 = _mm_load_ps(val);
        const __m128 floats1 = _mm_load_ps(val + 4);
        const __m128 floats2 = _mm_load_ps(val + 8);
        const __m128 floats3 = _mm_load_ps(val + 12);
        __m128i resultVec = _mm_setzero_si128();

        for (const auto border : borders) {
            const __m128 borderVec = _mm_set1_ps(border);
            const __m128i r0 = _mm_castps_si128(_mm_cmpgt_ps(floats0, borderVec));
            const __m128i r1 = _mm_castps_si128(_mm_cmpgt_ps(floats1, borderVec));
            const __m128i r2 = _mm_castps_si128(_mm_cmpgt_ps(floats2, borderVec));
            const __m128i r3 = _mm_castps_si128(_mm_cmpgt_ps(floats3, borderVec));
            const __m128i packed = _mm_packs_epi16(_mm_packs_epi32(r0, r1), _mm_packs_epi32(r2, r3));
            resultVec = _mm_add_epi8(resultVec, _mm_and_si128(packed, mask));
        }
        _mm_storeu_si128((__m128i*)(result + docId), resultVec);
    }
    for (size_t docId = docCount16; docId < docCount; ++docId) {
        const auto val = floatAccessor(start + docId);
        for (const auto border : borders) {
            result[docId] += (ui8)(val > border);
        }
    }
    result += docCount;
}

#endif

/**
* This function binarizes
*/
template<typename TFloatFeatureAccessor, typename TCatFeatureAccessor>
inline void BinarizeFeatures(
    const TFullModel& model,
    TFloatFeatureAccessor floatAccessor,
    TCatFeatureAccessor catFeatureAccessor,
    size_t start,
    size_t end,
    TArrayRef<ui8> result,
    TVector<int>& transposedHash,
    TVector<float>& ctrs
) {
    const auto docCount = end - start;
    ui8* resultPtr = result.data();
    std::fill(result.begin(), result.end(), 0);
    for (const auto& floatFeature : model.ObliviousTrees.FloatFeatures) {
        BinarizeFloats(docCount, [&floatFeature, floatAccessor](size_t index) { return floatAccessor(floatFeature, index); }, floatFeature.Borders, start, resultPtr);
    }
    auto catFeatureCount = model.ObliviousTrees.CatFeatures.size();
    if (catFeatureCount > 0) {
        for (size_t docId = 0; docId < docCount; ++docId) {
            auto idx = docId;
            for (size_t i = 0; i < catFeatureCount; ++i) {
                transposedHash[idx] = catFeatureAccessor(i, start + docId);
                idx += docCount;
            }
        }
        THashMap<int, int> catFeaturePackedIndexes;
        for (int i = 0; i < model.ObliviousTrees.CatFeatures.ysize(); ++i) {
            catFeaturePackedIndexes[model.ObliviousTrees.CatFeatures[i].FeatureIndex] = i;
        }
        OneHotBinsFromTransposedCatFeatures(model.ObliviousTrees.OneHotFeatures, catFeaturePackedIndexes, docCount, resultPtr, transposedHash);
        model.CtrProvider->CalcCtrs(
            model.ObliviousTrees.GetUsedModelCtrs(),
            result,
            transposedHash,
            docCount,
            ctrs
        );
        for (size_t i = 0; i < model.ObliviousTrees.CtrFeatures.size(); ++i) {
            const auto& ctr = model.ObliviousTrees.CtrFeatures[i];
            auto ctrFloatsPtr = &ctrs[i * docCount];
            BinarizeFloats(docCount, [ctrFloatsPtr](size_t index) { return ctrFloatsPtr[index]; }, ctr.Borders, 0, resultPtr);
        }
    }
}

using TCalcerIndexType = ui32;

using TTreeCalcFunction = std::function<void(
    const TFullModel& model,
    const ui8* __restrict binFeatures,
    size_t docCountInBlock,
    TCalcerIndexType* __restrict indexesVec,
    size_t treeStart,
    size_t treeEnd,
    double* __restrict results)>;

void CalcIndexes(
    bool needXorMask,
    const ui8* __restrict binFeatures,
    size_t docCountInBlock,
    ui32* __restrict indexesVec,
    const ui32* __restrict treeSplitsCurPtr,
    int curTreeSize);

TTreeCalcFunction GetCalcTreesFunction(const TFullModel& model, size_t docCountInBlock);

template<typename TFloatFeatureAccessor, typename TCatFeatureAccessor>
inline void CalcGeneric(
    const TFullModel& model,
    TFloatFeatureAccessor floatFeatureAccessor,
    TCatFeatureAccessor catFeaturesAccessor,
    size_t docCount,
    size_t treeStart,
    size_t treeEnd,
    TArrayRef<double> results)
{
    size_t blockSize = FORMULA_EVALUATION_BLOCK_SIZE;
    blockSize = Min(blockSize, docCount);
    TVector<ui8> binFeatures(blockSize * model.ObliviousTrees.GetEffectiveBinaryFeaturesBucketsCount());
    auto calcTrees = GetCalcTreesFunction(model, blockSize);
    if (docCount == 1) {
        CB_ENSURE((int)results.size() == model.ObliviousTrees.ApproxDimension);
        std::fill(results.begin(), results.end(), 0.0);
        TVector<int> transposedHash(model.ObliviousTrees.CatFeatures.size());
        TVector<float> ctrs(model.ObliviousTrees.GetUsedModelCtrs().size());
        BinarizeFeatures(
            model,
            floatFeatureAccessor,
            catFeaturesAccessor,
            0,
            1,
            binFeatures,
            transposedHash,
            ctrs
        );
        calcTrees(
                model,
                binFeatures.data(),
                1,
                nullptr,
                treeStart,
                treeEnd,
                results.data()
            );
        return;
    }

    CB_ENSURE(results.size() == docCount * model.ObliviousTrees.ApproxDimension);
    std::fill(results.begin(), results.end(), 0.0);
    TVector<TCalcerIndexType> indexesVec(blockSize);
    TVector<int> transposedHash(blockSize * model.ObliviousTrees.CatFeatures.size());
    TVector<float> ctrs(model.ObliviousTrees.GetUsedModelCtrs().size() * blockSize);
    for (size_t blockStart = 0; blockStart < docCount; blockStart += blockSize) {
        const auto docCountInBlock = Min(blockSize, docCount - blockStart);
        BinarizeFeatures(
            model,
            floatFeatureAccessor,
            catFeaturesAccessor,
            blockStart,
            blockStart + docCountInBlock,
            binFeatures,
            transposedHash,
            ctrs
        );
        calcTrees(
            model,
            binFeatures.data(),
            docCountInBlock,
            indexesVec.data(),
            treeStart,
            treeEnd,
            results.data() + blockStart * model.ObliviousTrees.ApproxDimension
        );
    }
}


/**
 * Warning: use aggressive caching. Stores all binarized features in RAM
 */
class TFeatureCachedTreeEvaluator {
public:
    template<typename TFloatFeatureAccessor,
             typename TCatFeatureAccessor>
    TFeatureCachedTreeEvaluator(const TFullModel& model,
                                TFloatFeatureAccessor floatFeatureAccessor,
                                TCatFeatureAccessor catFeaturesAccessor,
                                size_t docCount)
            : Model(model)
            , DocCount(docCount) {
        size_t blockSize = FORMULA_EVALUATION_BLOCK_SIZE;
        BlockSize = Min(blockSize, docCount);
        CalcFunction = GetCalcTreesFunction(Model, BlockSize);
        TVector<int> transposedHash(blockSize * model.ObliviousTrees.CatFeatures.size());
        TVector<float> ctrs(model.ObliviousTrees.GetUsedModelCtrs().size() * blockSize);
        {
            for (size_t blockStart = 0; blockStart < docCount; blockStart += blockSize) {
                const auto docCountInBlock = Min(blockSize, docCount - blockStart);
                TVector<ui8> binFeatures(model.ObliviousTrees.GetEffectiveBinaryFeaturesBucketsCount() * blockSize);
                BinarizeFeatures(
                        model,
                        floatFeatureAccessor,
                        catFeaturesAccessor,
                        blockStart,
                        blockStart + docCountInBlock,
                        binFeatures,
                        transposedHash,
                        ctrs
                );
                BinFeatures.push_back(std::move(binFeatures));
            }
        }
    }

    void Calc(size_t treeStart, size_t treeEnd, TArrayRef<double> results) const;
private:
    const TFullModel& Model;
    TVector<TVector<ui8>> BinFeatures;
    TTreeCalcFunction CalcFunction;
    ui64 DocCount;
    ui64 BlockSize;
};

template<typename TFloatFeatureAccessor, typename TCatFeatureAccessor>
inline TVector<TVector<double>> CalcTreeIntervalsGeneric(
    const TFullModel& model,
    TFloatFeatureAccessor floatFeatureAccessor,
    TCatFeatureAccessor catFeaturesAccessor,
    size_t docCount,
    size_t incrementStep)
{
    size_t blockSize = FORMULA_EVALUATION_BLOCK_SIZE;
    blockSize = Min(blockSize, docCount);
    auto treeStepCount = (model.ObliviousTrees.TreeSizes.size() + incrementStep - 1) / incrementStep;
    TVector<TVector<double>> results(docCount, TVector<double>(treeStepCount));
    CB_ENSURE(model.ObliviousTrees.ApproxDimension == 1);
    TVector<ui8> binFeatures(model.ObliviousTrees.GetEffectiveBinaryFeaturesBucketsCount() * blockSize);
    TVector<TCalcerIndexType> indexesVec(blockSize);
    TVector<int> transposedHash(blockSize * model.ObliviousTrees.CatFeatures.size());
    TVector<float> ctrs(model.ObliviousTrees.GetUsedModelCtrs().size() * blockSize);
    TVector<double> tmpResult(docCount);
    TArrayRef<double> tmpResultRef(tmpResult);
    auto calcTrees = GetCalcTreesFunction(model, blockSize);
    for (size_t blockStart = 0; blockStart < docCount; blockStart += blockSize) {
        const auto docCountInBlock = Min(blockSize, docCount - blockStart);
        BinarizeFeatures(
            model,
            floatFeatureAccessor,
            catFeaturesAccessor,
            blockStart,
            blockStart + docCountInBlock,
            binFeatures,
            transposedHash,
            ctrs
        );
        for (size_t stepIdx = 0; stepIdx < treeStepCount; ++stepIdx) {
            calcTrees(
                model,
                binFeatures.data(),
                docCountInBlock,
                indexesVec.data(),
                stepIdx * incrementStep,
                Min((stepIdx + 1) * incrementStep, model.ObliviousTrees.TreeSizes.size()),
                tmpResultRef.data() + blockStart * model.ObliviousTrees.ApproxDimension
            );
            for (size_t i = 0; i < docCountInBlock; ++i) {
                results[blockStart + i][stepIdx] = tmpResult[i];
            }
        }
    }
    return results;
}
