/******************************************************************************/
/* Skeletal mesh generation utilities for UE5.3                              */
/* -------------------------------------------------------------------------- */
/* License MIT                                                                */
/* Kindly sponsored by IMVU                                                   */
/* -------------------------------------------------------------------------- */
/* Runtime helpers to create or decompose a skeletal mesh from surface data.  */
/******************************************************************************/
#include "RuntimeSkeletalMeshGenerator.h"

#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshLODSettings.h"
#include "Engine/SkinnedAssetCommon.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODImporterData.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "SkeletalMeshTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogRuntimeSkeletalMeshGenerator, Log, All);

namespace
{
bool ValidateMeshInput(
	const USkeletalMesh* SkeletalMesh,
	const TArray<FMeshSurface>& Surfaces,
	const TArray<UMaterialInterface*>& SurfacesMaterial,
	int32& OutUVCount,
	int32& OutMaxBoneInfluences,
	int32& OutMaxBoneIndex,
	TArray<int32>& OutSurfaceMaxBoneInfluences)
{
	if (SkeletalMesh == nullptr)
	{
		UE_LOG(LogRuntimeSkeletalMeshGenerator, Warning, TEXT("GenerateSkeletalMesh requires a valid SkeletalMesh."));
		return false;
	}

	const USkeleton* Skeleton = SkeletalMesh->GetSkeleton();
	if (Skeleton == nullptr)
	{
		UE_LOG(LogRuntimeSkeletalMeshGenerator, Warning, TEXT("GenerateSkeletalMesh requires SkeletalMesh->GetSkeleton() to be valid."));
		return false;
	}

	if (Surfaces.IsEmpty())
	{
		UE_LOG(LogRuntimeSkeletalMeshGenerator, Warning, TEXT("GenerateSkeletalMesh requires at least one surface."));
		return false;
	}

	OutUVCount = INDEX_NONE;
	OutMaxBoneInfluences = 0;
	OutMaxBoneIndex = 0;
	OutSurfaceMaxBoneInfluences.SetNumZeroed(Surfaces.Num());

	const FReferenceSkeleton& ReferenceSkeleton = Skeleton->GetReferenceSkeleton();

	for (int32 SurfaceIndex = 0; SurfaceIndex < Surfaces.Num(); ++SurfaceIndex)
	{
		const FMeshSurface& Surface = Surfaces[SurfaceIndex];
		const int32 VertexCount = Surface.Vertices.Num();

		if (VertexCount <= 0)
		{
			UE_LOG(LogRuntimeSkeletalMeshGenerator, Warning, TEXT("Surface %d has no vertices."), SurfaceIndex);
			return false;
		}

		if (Surface.Indices.IsEmpty() || (Surface.Indices.Num() % 3) != 0)
		{
			UE_LOG(LogRuntimeSkeletalMeshGenerator, Warning, TEXT("Surface %d indices must be a non-empty triangle list."), SurfaceIndex);
			return false;
		}

		if (Surface.MaterialIndex < 0 || !SurfacesMaterial.IsValidIndex(Surface.MaterialIndex))
		{
			UE_LOG(
				LogRuntimeSkeletalMeshGenerator,
				Warning,
				TEXT("Surface %d references invalid material slot %d."),
				SurfaceIndex,
				Surface.MaterialIndex);
			return false;
		}

		if (Surface.Tangents.Num() != VertexCount ||
			Surface.Normals.Num() != VertexCount ||
			Surface.FlipBinormalSigns.Num() != VertexCount ||
			Surface.Uvs.Num() != VertexCount ||
			Surface.BoneInfluences.Num() != VertexCount)
		{
			UE_LOG(LogRuntimeSkeletalMeshGenerator, Warning, TEXT("Surface %d vertex attribute counts do not match Vertices.Num()."), SurfaceIndex);
			return false;
		}

		if (!Surface.Colors.IsEmpty() && Surface.Colors.Num() != VertexCount)
		{
			UE_LOG(LogRuntimeSkeletalMeshGenerator, Warning, TEXT("Surface %d Colors count does not match Vertices.Num()."), SurfaceIndex);
			return false;
		}

		int32 SurfaceMaxBoneInfluences = 0;
		for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
		{
			const TArray<FVector2D>& VertexUVs = Surface.Uvs[VertexIndex];
			if (OutUVCount == INDEX_NONE)
			{
				OutUVCount = VertexUVs.Num();
			}
			else if (OutUVCount != VertexUVs.Num())
			{
				UE_LOG(LogRuntimeSkeletalMeshGenerator, Warning, TEXT("Surface %d vertex %d uses a different UV channel count."), SurfaceIndex, VertexIndex);
				return false;
			}

			if (VertexUVs.Num() > MAX_STATIC_TEXCOORDS)
			{
				UE_LOG(
					LogRuntimeSkeletalMeshGenerator,
					Warning,
					TEXT("Surface %d vertex %d uses %d UV channels, exceeding MAX_STATIC_TEXCOORDS."),
					SurfaceIndex,
					VertexIndex,
					VertexUVs.Num());
				return false;
			}

			const TArray<FRawBoneInfluence>& VertexInfluences = Surface.BoneInfluences[VertexIndex];
			if (VertexInfluences.IsEmpty())
			{
				UE_LOG(LogRuntimeSkeletalMeshGenerator, Warning, TEXT("Surface %d vertex %d has no bone influences."), SurfaceIndex, VertexIndex);
				return false;
			}

			if (VertexInfluences.Num() > MAX_TOTAL_INFLUENCES)
			{
				UE_LOG(
					LogRuntimeSkeletalMeshGenerator,
					Warning,
					TEXT("Surface %d vertex %d uses %d bone influences, exceeding MAX_TOTAL_INFLUENCES."),
					SurfaceIndex,
					VertexIndex,
					VertexInfluences.Num());
				return false;
			}

			SurfaceMaxBoneInfluences = FMath::Max(SurfaceMaxBoneInfluences, VertexInfluences.Num());

			for (const FRawBoneInfluence& Influence : VertexInfluences)
			{
				if (Influence.VertexIndex != VertexIndex)
				{
					UE_LOG(
						LogRuntimeSkeletalMeshGenerator,
						Warning,
						TEXT("Surface %d vertex %d contains an influence with mismatched VertexIndex %d."),
						SurfaceIndex,
						VertexIndex,
						Influence.VertexIndex);
					return false;
				}

				if (!FMath::IsFinite(Influence.Weight))
				{
					UE_LOG(LogRuntimeSkeletalMeshGenerator, Warning, TEXT("Surface %d vertex %d contains a non-finite bone weight."), SurfaceIndex, VertexIndex);
					return false;
				}

				if (!ReferenceSkeleton.IsValidIndex(Influence.BoneIndex))
				{
					UE_LOG(
						LogRuntimeSkeletalMeshGenerator,
						Warning,
						TEXT("Surface %d vertex %d references invalid bone index %d."),
						SurfaceIndex,
						VertexIndex,
						Influence.BoneIndex);
					return false;
				}

				OutMaxBoneIndex = FMath::Max(OutMaxBoneIndex, Influence.BoneIndex);
			}
		}

		for (uint32 Index : Surface.Indices)
		{
			if (Index >= static_cast<uint32>(VertexCount))
			{
				UE_LOG(
					LogRuntimeSkeletalMeshGenerator,
					Warning,
					TEXT("Surface %d contains vertex index %u outside the vertex range 0..%d."),
					SurfaceIndex,
					Index,
					VertexCount - 1);
				return false;
			}
		}

		OutSurfaceMaxBoneInfluences[SurfaceIndex] = SurfaceMaxBoneInfluences;
		OutMaxBoneInfluences = FMath::Max(OutMaxBoneInfluences, SurfaceMaxBoneInfluences);
	}

	if (OutUVCount == INDEX_NONE)
	{
		OutUVCount = 0;
	}

	return OutMaxBoneInfluences > 0;
}

FReferenceSkeleton BuildReferenceSkeletonWithOverrides(
	const USkeleton* Skeleton,
	const TMap<FName, FTransform>& BoneTransformsOverride)
{
	FReferenceSkeleton ReferenceSkeleton = Skeleton->GetReferenceSkeleton();
	if (BoneTransformsOverride.IsEmpty())
	{
		return ReferenceSkeleton;
	}

	FReferenceSkeletonModifier ReferenceSkeletonModifier(ReferenceSkeleton, Skeleton);
	for (const TPair<FName, FTransform>& Pair : BoneTransformsOverride)
	{
		const int32 BoneIndex = ReferenceSkeletonModifier.FindBoneIndex(Pair.Key);
		if (BoneIndex == INDEX_NONE)
		{
			UE_LOG(LogRuntimeSkeletalMeshGenerator, Warning, TEXT("Bone override '%s' was ignored because the bone does not exist."), *Pair.Key.ToString());
			continue;
		}

		ReferenceSkeletonModifier.UpdateRefPoseTransform(BoneIndex, Pair.Value);
	}

	return ReferenceSkeleton;
}

void ResetSkeletalMeshState(USkeletalMesh* SkeletalMesh)
{
	SkeletalMesh->ReleaseResources();
	SkeletalMesh->GetMaterials().Reset();
	SkeletalMesh->GetRefBasesInvMatrix().Reset();
	SkeletalMesh->ResetLODInfo();

#if WITH_EDITORONLY_DATA
	if (FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel())
	{
		ImportedModel->LODModels.Reset();
	}
#endif
}

void BuildSectionBoneMaps(
	const USkeleton* Skeleton,
	const TArray<FMeshSurface>& Surfaces,
	TArray<TArray<FBoneIndexType>>& OutSectionBoneMaps,
	TArray<TMap<int32, FBoneIndexType>>& OutSectionBoneMapIndices)
{
	const FReferenceSkeleton& ReferenceSkeleton = Skeleton->GetReferenceSkeleton();
	const int32 BoneCount = ReferenceSkeleton.GetRawBoneNum();

	OutSectionBoneMaps.SetNum(Surfaces.Num());
	OutSectionBoneMapIndices.SetNum(Surfaces.Num());

	for (int32 SurfaceIndex = 0; SurfaceIndex < Surfaces.Num(); ++SurfaceIndex)
	{
		TSet<int32> UsedBoneIndices;
		for (const TArray<FRawBoneInfluence>& VertexInfluences : Surfaces[SurfaceIndex].BoneInfluences)
		{
			for (const FRawBoneInfluence& Influence : VertexInfluences)
			{
				if (FMath::IsNearlyZero(Influence.Weight))
				{
					continue;
				}

				for (int32 BoneIndex = Influence.BoneIndex; BoneIndex != INDEX_NONE; BoneIndex = ReferenceSkeleton.GetParentIndex(BoneIndex))
				{
					UsedBoneIndices.Add(BoneIndex);
				}
			}
		}

		TArray<FBoneIndexType>& BoneMap = OutSectionBoneMaps[SurfaceIndex];
		if (UsedBoneIndices.IsEmpty())
		{
			BoneMap.Reserve(BoneCount);
			for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
			{
				BoneMap.Add(static_cast<FBoneIndexType>(BoneIndex));
			}
		}
		else
		{
			BoneMap.Reserve(UsedBoneIndices.Num());
			for (int32 BoneIndex : UsedBoneIndices)
			{
				BoneMap.Add(static_cast<FBoneIndexType>(BoneIndex));
			}
			BoneMap.Sort();
		}

		TMap<int32, FBoneIndexType>& BoneMapIndexLookup = OutSectionBoneMapIndices[SurfaceIndex];
		for (int32 BoneMapIndex = 0; BoneMapIndex < BoneMap.Num(); ++BoneMapIndex)
		{
			BoneMapIndexLookup.Add(BoneMap[BoneMapIndex], static_cast<FBoneIndexType>(BoneMapIndex));
		}
	}
}

void CollectRequiredBones(
	const TArray<TArray<FBoneIndexType>>& SectionBoneMaps,
	const int32 BoneCount,
	TArray<FBoneIndexType>& OutRequiredBones)
{
	TSet<int32> UniqueBoneIndices;
	for (const TArray<FBoneIndexType>& SectionBoneMap : SectionBoneMaps)
	{
		for (FBoneIndexType BoneIndex : SectionBoneMap)
		{
			UniqueBoneIndices.Add(BoneIndex);
		}
	}

	if (UniqueBoneIndices.IsEmpty())
	{
		OutRequiredBones.Reserve(BoneCount);
		for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
		{
			OutRequiredBones.Add(static_cast<FBoneIndexType>(BoneIndex));
		}
		return;
	}

	OutRequiredBones.Reserve(UniqueBoneIndices.Num());
	for (int32 BoneIndex : UniqueBoneIndices)
	{
		OutRequiredBones.Add(static_cast<FBoneIndexType>(BoneIndex));
	}
	OutRequiredBones.Sort();
}
} // namespace

void FRuntimeSkeletalMeshGeneratorModule::StartupModule()
{
}

void FRuntimeSkeletalMeshGeneratorModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FRuntimeSkeletalMeshGeneratorModule, RuntimeSkeletalMeshGenerator)

bool FRuntimeSkeletalMeshGenerator::GenerateSkeletalMesh(
	USkeletalMesh* SkeletalMesh,
	const TArray<FMeshSurface>& Surfaces,
	const TArray<UMaterialInterface*>& SurfacesMaterial,
	const bool bNeedCPUAccess,
	const TMap<FName, FTransform>& BoneTransformsOverride)
{
	int32 UVCount = 0;
	int32 MaxBoneInfluences = 0;
	int32 MaxBoneIndex = 0;
	TArray<int32> SurfaceMaxBoneInfluences;

	if (!ValidateMeshInput(
		SkeletalMesh,
		Surfaces,
		SurfacesMaterial,
		UVCount,
		MaxBoneInfluences,
		MaxBoneIndex,
		SurfaceMaxBoneInfluences))
	{
		return false;
	}

	FlushRenderingCommands();

	const USkeleton* Skeleton = SkeletalMesh->GetSkeleton();
	const FReferenceSkeleton ReferenceSkeleton = BuildReferenceSkeletonWithOverrides(Skeleton, BoneTransformsOverride);
	SkeletalMesh->SetRefSkeleton(ReferenceSkeleton);

#if WITH_EDITOR
	TUniquePtr<FScopedSkeletalMeshPostEditChange> ScopedPostEditChange;
	if (GIsEditor)
	{
		ScopedPostEditChange = MakeUnique<FScopedSkeletalMeshPostEditChange>(SkeletalMesh, false, true);
	}
#endif

	ResetSkeletalMeshState(SkeletalMesh);

	constexpr int32 LODIndex = 0;

#if WITH_EDITORONLY_DATA
	FSkeletalMeshImportData ImportedModelData;
	ImportedModelData.Influences.Reserve(Surfaces.Num() * MaxBoneInfluences);
#endif

	TArray<uint32> SurfaceVertexOffsets;
	TArray<uint32> SurfaceIndexOffsets;
	SurfaceVertexOffsets.SetNum(Surfaces.Num());
	SurfaceIndexOffsets.SetNum(Surfaces.Num());

	const bool bUse16BitBoneIndex = MaxBoneIndex <= MAX_uint16;

	TArray<TArray<FBoneIndexType>> SectionBoneMaps;
	TArray<TMap<int32, FBoneIndexType>> SectionBoneMapIndices;
	BuildSectionBoneMaps(Skeleton, Surfaces, SectionBoneMaps, SectionBoneMapIndices);

	TArray<FStaticMeshBuildVertex> StaticVertices;
	TArray<FVector> Vertices;
	TArray<uint32> Indices;
	TArray<int32> VertexMaterialIndices;

	{
		uint32 VertexCount = 0;
		uint32 IndexCount = 0;
		for (const FMeshSurface& Surface : Surfaces)
		{
			VertexCount += Surface.Vertices.Num();
			IndexCount += Surface.Indices.Num();
		}

		StaticVertices.SetNumZeroed(VertexCount);
		Vertices.SetNum(VertexCount);
		VertexMaterialIndices.SetNum(VertexCount);
		Indices.SetNum(IndexCount);

		uint32 VertexOffset = 0;
		uint32 IndexOffset = 0;
		for (int32 SurfaceIndex = 0; SurfaceIndex < Surfaces.Num(); ++SurfaceIndex)
		{
			const FMeshSurface& Surface = Surfaces[SurfaceIndex];

			for (int32 LocalVertexIndex = 0; LocalVertexIndex < Surface.Vertices.Num(); ++LocalVertexIndex)
			{
				FStaticMeshBuildVertex& StaticVertex = StaticVertices[VertexOffset + LocalVertexIndex];
				StaticVertex.Color = Surface.Colors.IsEmpty() ? FColor::White : Surface.Colors[LocalVertexIndex];
				StaticVertex.Position = FVector3f(Surface.Vertices[LocalVertexIndex]);
				StaticVertex.TangentX = FVector3f(Surface.Tangents[LocalVertexIndex]);
				StaticVertex.TangentY = FVector3f(
					FVector::CrossProduct(Surface.Normals[LocalVertexIndex], Surface.Tangents[LocalVertexIndex]) *
					(Surface.FlipBinormalSigns[LocalVertexIndex] ? -1.0 : 1.0));
				StaticVertex.TangentZ = FVector3f(Surface.Normals[LocalVertexIndex]);

				for (int32 UVIndex = 0; UVIndex < UVCount; ++UVIndex)
				{
					StaticVertex.UVs[UVIndex] = FVector2f(Surface.Uvs[LocalVertexIndex][UVIndex]);
				}

				VertexMaterialIndices[VertexOffset + LocalVertexIndex] = Surface.MaterialIndex;
			}

			FMemory::Memcpy(
				Vertices.GetData() + VertexOffset,
				Surface.Vertices.GetData(),
				sizeof(FVector) * Surface.Vertices.Num());

			for (int32 LocalIndex = 0; LocalIndex < Surface.Indices.Num(); ++LocalIndex)
			{
				Indices[IndexOffset + LocalIndex] = Surface.Indices[LocalIndex] + VertexOffset;
			}

			SurfaceVertexOffsets[SurfaceIndex] = VertexOffset;
			SurfaceIndexOffsets[SurfaceIndex] = IndexOffset;
			VertexOffset += Surface.Vertices.Num();
			IndexOffset += Surface.Indices.Num();
		}
	}

#if WITH_EDITORONLY_DATA
	ImportedModelData.Points.Append(Vertices);
	ImportedModelData.PointToRawMap.AddUninitialized(ImportedModelData.Points.Num());
	for (int32 PointIndex = 0; PointIndex < ImportedModelData.Points.Num(); ++PointIndex)
	{
		ImportedModelData.PointToRawMap[PointIndex] = PointIndex;
	}

	ImportedModelData.Materials.SetNum(SurfacesMaterial.Num());
	for (int32 MaterialIndex = 0; MaterialIndex < SurfacesMaterial.Num(); ++MaterialIndex)
	{
		SkeletalMeshImportData::FMaterial& ImportedMaterial = ImportedModelData.Materials[MaterialIndex];
		ImportedMaterial.Material = SurfacesMaterial[MaterialIndex];
		ImportedMaterial.MaterialImportName =
			SurfacesMaterial[MaterialIndex] != nullptr ? SurfacesMaterial[MaterialIndex]->GetFullName() : FString();
	}

	ImportedModelData.Faces.SetNum(Indices.Num() / 3);
	for (int32 FaceIndex = 0; FaceIndex < ImportedModelData.Faces.Num(); ++FaceIndex)
	{
		SkeletalMeshImportData::FTriangle& Triangle = ImportedModelData.Faces[FaceIndex];

		const int32 VertexIndex0 = Indices[FaceIndex * 3 + 0];
		const int32 VertexIndex1 = Indices[FaceIndex * 3 + 1];
		const int32 VertexIndex2 = Indices[FaceIndex * 3 + 2];

		Triangle.WedgeIndex[0] = FaceIndex * 3 + 0;
		Triangle.WedgeIndex[1] = FaceIndex * 3 + 1;
		Triangle.WedgeIndex[2] = FaceIndex * 3 + 2;

		Triangle.TangentX[0] = StaticVertices[VertexIndex0].TangentX;
		Triangle.TangentY[0] = StaticVertices[VertexIndex0].TangentY;
		Triangle.TangentZ[0] = StaticVertices[VertexIndex0].TangentZ;

		Triangle.TangentX[1] = StaticVertices[VertexIndex1].TangentX;
		Triangle.TangentY[1] = StaticVertices[VertexIndex1].TangentY;
		Triangle.TangentZ[1] = StaticVertices[VertexIndex1].TangentZ;

		Triangle.TangentX[2] = StaticVertices[VertexIndex2].TangentX;
		Triangle.TangentY[2] = StaticVertices[VertexIndex2].TangentY;
		Triangle.TangentZ[2] = StaticVertices[VertexIndex2].TangentZ;

		Triangle.MatIndex = VertexMaterialIndices[VertexIndex0];
		Triangle.AuxMatIndex = 0;
		Triangle.SmoothingGroups = 1;
	}

	ImportedModelData.Wedges.SetNum(ImportedModelData.Faces.Num() * 3);
	for (int32 FaceIndex = 0; FaceIndex < ImportedModelData.Faces.Num(); ++FaceIndex)
	{
		for (int32 CornerIndex = 0; CornerIndex < 3; ++CornerIndex)
		{
			const int32 WedgeIndex = FaceIndex * 3 + CornerIndex;
			const int32 VertexIndex = Indices[WedgeIndex];

			ImportedModelData.Wedges[WedgeIndex].VertexIndex = VertexIndex;
			for (int32 UVIndex = 0; UVIndex < FMath::Min<int32>(MAX_TEXCOORDS, MAX_STATIC_TEXCOORDS); ++UVIndex)
			{
				ImportedModelData.Wedges[WedgeIndex].UVs[UVIndex] = StaticVertices[VertexIndex].UVs[UVIndex];
			}
			ImportedModelData.Wedges[WedgeIndex].MatIndex = VertexMaterialIndices[VertexIndex];
			ImportedModelData.Wedges[WedgeIndex].Color = StaticVertices[VertexIndex].Color;
			ImportedModelData.Wedges[WedgeIndex].Reserved = 0;
		}
	}

	{
		const int32 BoneCount = SkeletalMesh->GetRefSkeleton().GetRawBoneNum();
		SkeletalMeshImportData::FBone DefaultBone;
		DefaultBone.Name = FString(TEXT(""));
		DefaultBone.Flags = 0;
		DefaultBone.NumChildren = 0;
		DefaultBone.ParentIndex = INDEX_NONE;
		DefaultBone.BonePos.Transform.SetIdentity();
		DefaultBone.BonePos.Length = 0.0;
		DefaultBone.BonePos.XSize = 1.0;
		DefaultBone.BonePos.YSize = 1.0;
		DefaultBone.BonePos.ZSize = 1.0;
		ImportedModelData.RefBonesBinary.Init(DefaultBone, BoneCount);
		for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
		{
			ImportedModelData.RefBonesBinary[BoneIndex].Name = SkeletalMesh->GetRefSkeleton().GetBoneName(BoneIndex).ToString();
			ImportedModelData.RefBonesBinary[BoneIndex].ParentIndex = SkeletalMesh->GetRefSkeleton().GetParentIndex(BoneIndex);
			if (ImportedModelData.RefBonesBinary[BoneIndex].ParentIndex != INDEX_NONE)
			{
				ImportedModelData.RefBonesBinary[ImportedModelData.RefBonesBinary[BoneIndex].ParentIndex].NumChildren += 1;
			}
		}

		for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
		{
			ImportedModelData.RefBonesBinary[BoneIndex].BonePos.Transform =
				FTransform3f(SkeletalMesh->GetRefSkeleton().GetRawRefBonePose()[BoneIndex]);
			ImportedModelData.RefBonesBinary[BoneIndex].BonePos.Length =
				ImportedModelData.RefBonesBinary[BoneIndex].BonePos.Transform.GetLocation().Size();
		}
	}
#endif

	SkeletalMesh->AllocateResourceForRendering();
	FSkeletalMeshRenderData* MeshRenderData = SkeletalMesh->GetResourceForRendering();
	if (MeshRenderData == nullptr)
	{
		UE_LOG(LogRuntimeSkeletalMeshGenerator, Warning, TEXT("Failed to allocate skeletal mesh render data."));
		return false;
	}

	FSkeletalMeshLODRenderData* LODMeshRenderData = new FSkeletalMeshLODRenderData();
	if (LODMeshRenderData == nullptr)
	{
		UE_LOG(LogRuntimeSkeletalMeshGenerator, Warning, TEXT("Failed to allocate skeletal mesh LOD render data."));
		return false;
	}
	MeshRenderData->LODRenderData.Add(LODMeshRenderData);

	FSkeletalMeshLODInfo& MeshLodInfo = SkeletalMesh->AddLODInfo();
	MeshLodInfo.LODHysteresis = 0.02f;
	MeshLodInfo.ScreenSize = 1.0f;
	MeshLodInfo.bAllowCPUAccess = bNeedCPUAccess;
	if (bNeedCPUAccess)
	{
		MeshLodInfo.SkinCacheUsage = ESkinCacheUsage::Disabled;
		MeshLodInfo.bHasBeenSimplified = true;
	}

	const FBox BoundingBox(Vertices.GetData(), Vertices.Num());
	SkeletalMesh->SetImportedBounds(FBoxSphereBounds(BoundingBox));

#if WITH_EDITORONLY_DATA
	FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel();
	if (ImportedModel == nullptr)
	{
		UE_LOG(LogRuntimeSkeletalMeshGenerator, Warning, TEXT("GenerateSkeletalMesh requires imported model support in editor builds."));
		return false;
	}

	FSkeletalMeshLODModel* SkeletalMeshLODModel = new FSkeletalMeshLODModel();
	ImportedModel->LODModels.Add(SkeletalMeshLODModel);

	SkeletalMeshLODModel->NumVertices = Vertices.Num();
	SkeletalMeshLODModel->NumTexCoords = UVCount;
	SkeletalMeshLODModel->Sections.SetNum(Surfaces.Num());
	SkeletalMeshLODModel->MaxImportVertex = Vertices.Num() - 1;

	bool bHasVertexColors = false;
	for (const FMeshSurface& Surface : Surfaces)
	{
		bHasVertexColors |= !Surface.Colors.IsEmpty();
	}

	ImportedModelData.NumTexCoords = UVCount;
	ImportedModelData.MaxMaterialIndex = SurfacesMaterial.IsEmpty() ? 0 : (SurfacesMaterial.Num() - 1);
	ImportedModelData.bHasVertexColors = bHasVertexColors;
	ImportedModelData.bHasNormals = true;
	ImportedModelData.bHasTangents = true;
	ImportedModelData.bUseT0AsRefPose = false;
	ImportedModelData.bDiffPose = false;
#endif

	LODMeshRenderData->RenderSections.SetNum(Surfaces.Num());

	for (int32 SurfaceIndex = 0; SurfaceIndex < Surfaces.Num(); ++SurfaceIndex)
	{
		const FMeshSurface& Surface = Surfaces[SurfaceIndex];
		const TArray<FBoneIndexType>& SectionBoneMap = SectionBoneMaps[SurfaceIndex];
		const TMap<int32, FBoneIndexType>& SectionBoneIndexLookup = SectionBoneMapIndices[SurfaceIndex];
		const int32 SectionMaxBoneInfluences = SurfaceMaxBoneInfluences[SurfaceIndex];

		FSkelMeshRenderSection& RenderSection = LODMeshRenderData->RenderSections[SurfaceIndex];
		RenderSection.bDisabled = false;
		RenderSection.BaseVertexIndex = SurfaceVertexOffsets[SurfaceIndex];
		RenderSection.NumVertices = Surface.Vertices.Num();
		RenderSection.BaseIndex = SurfaceIndexOffsets[SurfaceIndex];
		RenderSection.NumTriangles = Surface.Indices.Num() / 3;
		RenderSection.MaterialIndex = Surface.MaterialIndex;
		RenderSection.bCastShadow = true;
		RenderSection.bRecomputeTangent = false;
		RenderSection.MaxBoneInfluences = SectionMaxBoneInfluences;
		RenderSection.BoneMap = SectionBoneMap;

#if WITH_EDITORONLY_DATA
		FSkelMeshSection& MeshSection = SkeletalMeshLODModel->Sections[SurfaceIndex];
		MeshSection.bDisabled = RenderSection.bDisabled;
		MeshSection.bRecomputeTangent = RenderSection.bRecomputeTangent;
		MeshSection.bCastShadow = RenderSection.bCastShadow;
		MeshSection.BaseVertexIndex = RenderSection.BaseVertexIndex;
		MeshSection.BaseIndex = RenderSection.BaseIndex;
		MeshSection.MaterialIndex = RenderSection.MaterialIndex;
		MeshSection.NumVertices = RenderSection.NumVertices;
		MeshSection.NumTriangles = RenderSection.NumTriangles;
		MeshSection.MaxBoneInfluences = RenderSection.MaxBoneInfluences;
		MeshSection.bUse16BitBoneIndex = bUse16BitBoneIndex;
		MeshSection.OriginalDataSectionIndex = SurfaceIndex;
		MeshSection.BoneMap = SectionBoneMap;

		MeshSection.SoftVertices.SetNumZeroed(Surface.Vertices.Num());
		for (int32 LocalVertexIndex = 0; LocalVertexIndex < Surface.Vertices.Num(); ++LocalVertexIndex)
		{
			FSoftSkinVertex& SoftVertex = MeshSection.SoftVertices[LocalVertexIndex];
			SoftVertex.Color = Surface.Colors.IsEmpty() ? FColor::White : Surface.Colors[LocalVertexIndex];
			SoftVertex.Position = FVector3f(Surface.Vertices[LocalVertexIndex]);
			SoftVertex.TangentX = FVector3f(Surface.Tangents[LocalVertexIndex]);
			SoftVertex.TangentY = FVector3f(
				FVector::CrossProduct(Surface.Normals[LocalVertexIndex], Surface.Tangents[LocalVertexIndex]) *
				(Surface.FlipBinormalSigns[LocalVertexIndex] ? -1.0 : 1.0));
			SoftVertex.TangentZ = FVector4f(FVector3f(Surface.Normals[LocalVertexIndex]), 0.0f);

			for (int32 UVIndex = 0; UVIndex < UVCount; ++UVIndex)
			{
				SoftVertex.UVs[UVIndex] = FVector2f(Surface.Uvs[LocalVertexIndex][UVIndex]);
			}

			const TArray<FRawBoneInfluence>& VertexInfluences = Surface.BoneInfluences[LocalVertexIndex];
			FMemory::Memset(SoftVertex.InfluenceWeights, 0, sizeof(SoftVertex.InfluenceWeights));
			FMemory::Memset(SoftVertex.InfluenceBones, 0, sizeof(SoftVertex.InfluenceBones));

			const int32 InfluenceCount = FMath::Min(VertexInfluences.Num(), MAX_TOTAL_INFLUENCES);
			for (int32 InfluenceIndex = 0; InfluenceIndex < InfluenceCount; ++InfluenceIndex)
			{
				const FRawBoneInfluence& VertexInfluence = VertexInfluences[InfluenceIndex];
				const uint16 EncodedWeight = static_cast<uint16>(FMath::Clamp(VertexInfluence.Weight, 0.0f, 1.0f) * 65535.0f);
				SoftVertex.InfluenceWeights[InfluenceIndex] = EncodedWeight;

				if (EncodedWeight == 0)
				{
					SoftVertex.InfluenceBones[InfluenceIndex] = 0;
					continue;
				}

				const FBoneIndexType* BoneMapIndex = SectionBoneIndexLookup.Find(VertexInfluence.BoneIndex);
				if (BoneMapIndex == nullptr)
				{
					UE_LOG(
						LogRuntimeSkeletalMeshGenerator,
						Warning,
						TEXT("Section %d does not contain bone %d in its bone map."),
						SurfaceIndex,
						VertexInfluence.BoneIndex);
					return false;
				}

				SoftVertex.InfluenceBones[InfluenceIndex] = *BoneMapIndex;
			}
		}

		FSkelMeshSourceSectionUserData& UserSectionData = SkeletalMeshLODModel->UserSectionsData.FindOrAdd(SurfaceIndex);
		UserSectionData.bDisabled = MeshSection.bDisabled;
		UserSectionData.bCastShadow = MeshSection.bCastShadow;
		UserSectionData.bRecomputeTangent = MeshSection.bRecomputeTangent;
		UserSectionData.RecomputeTangentsVertexMaskChannel = MeshSection.RecomputeTangentsVertexMaskChannel;
		UserSectionData.GenerateUpToLodIndex = MeshSection.GenerateUpToLodIndex;
#endif

		RenderSection.DuplicatedVerticesBuffer.DupVertData.ResizeBuffer(1);
		uint8* VertData = RenderSection.DuplicatedVerticesBuffer.DupVertData.GetDataPointer();
		FMemory::Memzero(VertData, sizeof(uint32) * RenderSection.DuplicatedVerticesBuffer.DupVertData.Num());

		RenderSection.DuplicatedVerticesBuffer.DupVertIndexData.ResizeBuffer(RenderSection.NumVertices);
		uint8* IndexData = RenderSection.DuplicatedVerticesBuffer.DupVertIndexData.GetDataPointer();
		FMemory::Memzero(IndexData, RenderSection.NumVertices * sizeof(FIndexLengthPair));
	}

	{
#if WITH_EDITORONLY_DATA
		SkeletalMeshLODModel->IndexBuffer = Indices;
#endif

		LODMeshRenderData->MultiSizeIndexContainer.RebuildIndexBuffer(
			Indices.Num() < MAX_uint16 ? sizeof(uint16) : sizeof(uint32),
			Indices);
	}

	LODMeshRenderData->StaticVertexBuffers.PositionVertexBuffer.Init(StaticVertices, bNeedCPUAccess);
	LODMeshRenderData->StaticVertexBuffers.ColorVertexBuffer.Init(StaticVertices, bNeedCPUAccess);
	LODMeshRenderData->StaticVertexBuffers.StaticMeshVertexBuffer.Init(StaticVertices, UVCount, bNeedCPUAccess);

	LODMeshRenderData->SkinWeightVertexBuffer.SetMaxBoneInfluences(MaxBoneInfluences);
	LODMeshRenderData->SkinWeightVertexBuffer.SetUse16BitBoneIndex(bUse16BitBoneIndex);
	LODMeshRenderData->SkinWeightVertexBuffer.SetNeedsCPUAccess(bNeedCPUAccess);

	TArray<FSkinWeightInfo> Weights;
	Weights.SetNumZeroed(Vertices.Num());

	for (int32 SurfaceIndex = 0; SurfaceIndex < Surfaces.Num(); ++SurfaceIndex)
	{
		const FMeshSurface& Surface = Surfaces[SurfaceIndex];
		const TMap<int32, FBoneIndexType>& SectionBoneIndexLookup = SectionBoneMapIndices[SurfaceIndex];

		for (int32 LocalVertexIndex = 0; LocalVertexIndex < Surface.BoneInfluences.Num(); ++LocalVertexIndex)
		{
			const int32 VertexIndex = SurfaceVertexOffsets[SurfaceIndex] + LocalVertexIndex;
			FSkinWeightInfo& WeightInfo = Weights[VertexIndex];
			const TArray<FRawBoneInfluence>& VertexInfluences = Surface.BoneInfluences[LocalVertexIndex];

			for (int32 InfluenceIndex = 0; InfluenceIndex < VertexInfluences.Num(); ++InfluenceIndex)
			{
				const FRawBoneInfluence& VertexInfluence = VertexInfluences[InfluenceIndex];
				const uint16 EncodedWeight = static_cast<uint16>(FMath::Clamp(VertexInfluence.Weight, 0.0f, 1.0f) * 65535.0f);
				WeightInfo.InfluenceWeights[InfluenceIndex] = EncodedWeight;

				if (EncodedWeight == 0)
				{
					WeightInfo.InfluenceBones[InfluenceIndex] = 0;
					continue;
				}

				const FBoneIndexType* BoneMapIndex = SectionBoneIndexLookup.Find(VertexInfluence.BoneIndex);
				if (BoneMapIndex == nullptr)
				{
					UE_LOG(
						LogRuntimeSkeletalMeshGenerator,
						Warning,
						TEXT("Vertex %d in surface %d references bone %d that is missing from the section bone map."),
						LocalVertexIndex,
						SurfaceIndex,
						VertexInfluence.BoneIndex);
					return false;
				}

				WeightInfo.InfluenceBones[InfluenceIndex] = *BoneMapIndex;

#if WITH_EDITORONLY_DATA
				SkeletalMeshImportData::FRawBoneInfluence& ImportedInfluence = ImportedModelData.Influences.AddDefaulted_GetRef();
				ImportedInfluence.Weight = static_cast<float>(EncodedWeight) / 65535.0f;
				ImportedInfluence.BoneIndex = VertexInfluence.BoneIndex;
				ImportedInfluence.VertexIndex = VertexIndex;
#endif
			}
		}
	}

	TArray<FBoneIndexType> RequiredBones;
	CollectRequiredBones(SectionBoneMaps, SkeletalMesh->GetRefSkeleton().GetRawBoneNum(), RequiredBones);

#if WITH_EDITORONLY_DATA
	SkeletalMeshLODModel->ActiveBoneIndices = RequiredBones;
	SkeletalMeshLODModel->RequiredBones = RequiredBones;
#endif
	LODMeshRenderData->RequiredBones = RequiredBones;
	LODMeshRenderData->ActiveBoneIndices = RequiredBones;
	LODMeshRenderData->SkinWeightVertexBuffer = Weights;

	SkeletalMesh->GetMaterials().Reset();
	SkeletalMesh->GetMaterials().Reserve(SurfacesMaterial.Num());
	for (UMaterialInterface* Material : SurfacesMaterial)
	{
		SkeletalMesh->GetMaterials().Emplace(Material);
	}

	SkeletalMesh->GetRefBasesInvMatrix().Reset();
	SkeletalMesh->CalculateInvRefMatrices();
	MeshRenderData->bReadyForStreaming = false;
	SkeletalMesh->NeverStream = bNeedCPUAccess;

#if WITH_EDITOR
	if (SkeletalMesh->GetLODSettings() != nullptr)
	{
		SkeletalMesh->GetLODSettings()->SetLODSettingsFromMesh(SkeletalMesh);

		const int32 NumSettings = FMath::Min(SkeletalMesh->GetLODSettings()->GetNumberOfSettings(), SkeletalMesh->GetLODNum());
		if (LODIndex < NumSettings)
		{
			const FSkeletalMeshLODGroupSettings& SkeletalMeshLODGroupSettings =
				SkeletalMesh->GetLODSettings()->GetSettingsForLODLevel(LODIndex);
			MeshLodInfo.BuildGUID = MeshLodInfo.ComputeDeriveDataCacheKey(&SkeletalMeshLODGroupSettings);
		}
	}

	const FString BuildStringID = SkeletalMesh->GetImportedModel()->LODModels[LODIndex].GetLODModelDeriveDataKey();
	SkeletalMesh->GetImportedModel()->LODModels[LODIndex].BuildStringID = BuildStringID;
	SkeletalMesh->SetLODImportedDataVersions(
		LODIndex,
		ESkeletalMeshGeoImportVersions::LatestVersion,
		ESkeletalMeshSkinningImportVersions::LatestVersion);
	SkeletalMesh->SaveLODImportedData(LODIndex, ImportedModelData);
	SkeletalMesh->InvalidateDeriveDataCacheGUID();
#endif

	SkeletalMesh->InitResources();
	return true;
}

USkeletalMeshComponent* FRuntimeSkeletalMeshGenerator::GenerateSkeletalMeshComponent(
	AActor* Actor,
	USkeleton* BaseSkeleton,
	const TArray<FMeshSurface>& Surfaces,
	const TArray<UMaterialInterface*>& SurfacesMaterial,
	const bool bNeedCPUAccess,
	const TMap<FName, FTransform>& BoneTransformsOverride)
{
	if (Actor == nullptr || BaseSkeleton == nullptr)
	{
		return nullptr;
	}

	const TObjectPtr<USkeletalMesh> SkeletalMesh = NewObject<USkeletalMesh>();
	if (!SkeletalMesh)
	{
		return nullptr;
	}

	SkeletalMesh->SetRefSkeleton(BaseSkeleton->GetReferenceSkeleton());
	SkeletalMesh->SetSkeleton(BaseSkeleton);

	if (!GenerateSkeletalMesh(
		SkeletalMesh,
		Surfaces,
		SurfacesMaterial,
		bNeedCPUAccess,
		BoneTransformsOverride))
	{
		return nullptr;
	}

	const TObjectPtr<USkeletalMeshComponent> SkeletalMeshComponent =
		NewObject<USkeletalMeshComponent>(Actor, USkeletalMeshComponent::StaticClass());
	if (!SkeletalMeshComponent)
	{
		return nullptr;
	}

	SkeletalMeshComponent->SetSkeletalMesh(SkeletalMesh);
	SkeletalMeshComponent->SetCPUSkinningEnabled(bNeedCPUAccess);

	if (Actor->GetRootComponent() != nullptr)
	{
		SkeletalMeshComponent->AttachToComponent(
			Actor->GetRootComponent(),
			FAttachmentTransformRules::KeepRelativeTransform);
	}
	else
	{
		Actor->SetRootComponent(SkeletalMeshComponent);
	}

	Actor->AddInstanceComponent(SkeletalMeshComponent);
	SkeletalMeshComponent->RegisterComponent();

	return SkeletalMeshComponent;
}

bool FRuntimeSkeletalMeshGenerator::UpdateSkeletalMeshComponent(
	USkeletalMeshComponent* SkeletalMeshComponent,
	USkeleton* BaseSkeleton,
	const TArray<FMeshSurface>& Surfaces,
	const TArray<UMaterialInterface*>& SurfacesMaterial,
	const bool bNeedCPUAccess,
	const TMap<FName, FTransform>& BoneTransformOverrides)
{
	if (SkeletalMeshComponent == nullptr || BaseSkeleton == nullptr)
	{
		return false;
	}

	const TObjectPtr<USkeletalMesh> SkeletalMesh = NewObject<USkeletalMesh>();
	if (!SkeletalMesh)
	{
		return false;
	}

	SkeletalMesh->SetRefSkeleton(BaseSkeleton->GetReferenceSkeleton());
	SkeletalMesh->SetSkeleton(BaseSkeleton);

	if (!GenerateSkeletalMesh(
		SkeletalMesh.Get(),
		Surfaces,
		SurfacesMaterial,
		bNeedCPUAccess,
		BoneTransformOverrides))
	{
		return false;
	}

	SkeletalMeshComponent->SetSkeletalMesh(SkeletalMesh);
	SkeletalMeshComponent->SetCPUSkinningEnabled(bNeedCPUAccess);
	return true;
}

bool FRuntimeSkeletalMeshGenerator::DecomposeSkeletalMesh(
	const USkeletalMesh* SkeletalMesh,
	TArray<FMeshSurface>& OutSurfaces,
	TArray<int32>& OutSurfacesVertexOffsets,
	TArray<int32>& OutSurfacesIndexOffsets,
	TArray<UMaterialInterface*>& OutSurfacesMaterial)
{
	OutSurfaces.Reset();
	OutSurfacesVertexOffsets.Reset();
	OutSurfacesIndexOffsets.Reset();
	OutSurfacesMaterial.Reset();

	if (SkeletalMesh == nullptr)
	{
		UE_LOG(LogRuntimeSkeletalMeshGenerator, Warning, TEXT("DecomposeSkeletalMesh requires a valid SkeletalMesh."));
		return false;
	}

	constexpr int32 LODIndex = 0;

	const FSkeletalMeshRenderData* RenderDataOwner = SkeletalMesh->GetResourceForRendering();
	if (RenderDataOwner == nullptr || !RenderDataOwner->LODRenderData.IsValidIndex(LODIndex))
	{
		UE_LOG(LogRuntimeSkeletalMeshGenerator, Warning, TEXT("SkeletalMesh has no render data for LOD 0."));
		return false;
	}

	const FSkeletalMeshLODRenderData& RenderData = RenderDataOwner->LODRenderData[LODIndex];
	const int32 RenderSectionsNum = RenderData.RenderSections.Num();
	if (RenderSectionsNum <= 0)
	{
		UE_LOG(LogRuntimeSkeletalMeshGenerator, Warning, TEXT("SkeletalMesh LOD 0 has no render sections."));
		return false;
	}

	OutSurfaces.SetNum(RenderSectionsNum);
	OutSurfacesVertexOffsets.SetNum(RenderSectionsNum);
	OutSurfacesIndexOffsets.SetNum(RenderSectionsNum);

	TArray<uint32> IndexBuffer;
	RenderData.MultiSizeIndexContainer.GetIndexBuffer(IndexBuffer);

	for (int32 SectionIndex = 0; SectionIndex < RenderSectionsNum; ++SectionIndex)
	{
		FMeshSurface& Surface = OutSurfaces[SectionIndex];
		const FSkelMeshRenderSection& RenderSection = RenderData.RenderSections[SectionIndex];

		Surface.MaterialIndex = RenderSection.MaterialIndex;

		const uint32 VertexIndexOffset = RenderSection.BaseVertexIndex;
		const uint32 VertexNum = RenderSection.NumVertices;
		OutSurfacesVertexOffsets[SectionIndex] = VertexIndexOffset;

		Surface.Vertices.SetNum(VertexNum);
		Surface.Normals.SetNum(VertexNum);
		Surface.Tangents.SetNum(VertexNum);
		Surface.FlipBinormalSigns.SetNum(VertexNum);
		Surface.Uvs.SetNum(VertexNum);
		Surface.Colors.SetNum(VertexNum);
		Surface.BoneInfluences.SetNum(VertexNum);

		for (uint32 LocalVertexIndex = 0; LocalVertexIndex < VertexNum; ++LocalVertexIndex)
		{
			const uint32 VertexIndex = VertexIndexOffset + LocalVertexIndex;

			Surface.Vertices[LocalVertexIndex] = FVector(RenderData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(VertexIndex));
			Surface.Normals[LocalVertexIndex] = FVector(RenderData.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(VertexIndex));
			Surface.Tangents[LocalVertexIndex] = FVector(RenderData.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentX(VertexIndex));

			const FVector ActualBinormal = FVector(RenderData.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentY(VertexIndex));
			const FVector CalculatedBinormal = FVector::CrossProduct(Surface.Normals[LocalVertexIndex], Surface.Tangents[LocalVertexIndex]);
			Surface.FlipBinormalSigns[LocalVertexIndex] = FVector::DotProduct(ActualBinormal, CalculatedBinormal) < 0.99f;

			Surface.Uvs[LocalVertexIndex].SetNum(RenderData.StaticVertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords());
			for (uint32 UVIndex = 0; UVIndex < RenderData.StaticVertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords(); ++UVIndex)
			{
				Surface.Uvs[LocalVertexIndex][UVIndex] =
					FVector2D(RenderData.StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(VertexIndex, UVIndex));
			}

			Surface.Colors[LocalVertexIndex] = FColor::White;
			if (VertexIndex < RenderData.StaticVertexBuffers.ColorVertexBuffer.GetNumVertices())
			{
				Surface.Colors[LocalVertexIndex] = RenderData.StaticVertexBuffers.ColorVertexBuffer.VertexColor(VertexIndex);
			}

			if (static_cast<int32>(RenderData.SkinWeightVertexBuffer.GetMaxBoneInfluences()) < RenderSection.MaxBoneInfluences)
			{
				UE_LOG(
					LogRuntimeSkeletalMeshGenerator,
					Warning,
					TEXT("Render section %d expects %d bone influences, but the skin weight buffer only exposes %d."),
					SectionIndex,
					RenderSection.MaxBoneInfluences,
					RenderData.SkinWeightVertexBuffer.GetMaxBoneInfluences());
				return false;
			}

			Surface.BoneInfluences[LocalVertexIndex].SetNum(RenderSection.MaxBoneInfluences);
			for (int32 BoneInfluenceIndex = 0; BoneInfluenceIndex < RenderSection.MaxBoneInfluences; ++BoneInfluenceIndex)
			{
				const int32 BoneMapIndex = RenderData.SkinWeightVertexBuffer.GetBoneIndex(VertexIndex, BoneInfluenceIndex);
				if (!RenderSection.BoneMap.IsValidIndex(BoneMapIndex))
				{
					UE_LOG(
						LogRuntimeSkeletalMeshGenerator,
						Warning,
						TEXT("Render section %d references invalid bone map index %d."),
						SectionIndex,
						BoneMapIndex);
					return false;
				}

				FRawBoneInfluence& BoneInfluence = Surface.BoneInfluences[LocalVertexIndex][BoneInfluenceIndex];
				BoneInfluence.VertexIndex = LocalVertexIndex;
				BoneInfluence.BoneIndex = RenderSection.BoneMap[BoneMapIndex];
				BoneInfluence.Weight = static_cast<float>(
					FMath::Clamp(RenderData.SkinWeightVertexBuffer.GetBoneWeight(VertexIndex, BoneInfluenceIndex) / 65535.0, 0.0, 1.0));
			}
		}

		const uint32 IndexIndexOffset = RenderSection.BaseIndex;
		const uint32 IndexCount = RenderSection.NumTriangles * 3;
		OutSurfacesIndexOffsets[SectionIndex] = IndexIndexOffset;
		Surface.Indices.SetNum(IndexCount);

		for (uint32 LocalIndex = 0; LocalIndex < IndexCount; ++LocalIndex)
		{
			const uint32 Index = LocalIndex + IndexIndexOffset;
			Surface.Indices[LocalIndex] = IndexBuffer[Index] - VertexIndexOffset;
		}
	}

	OutSurfacesMaterial.Reserve(SkeletalMesh->GetMaterials().Num());
	for (const FSkeletalMaterial& Material : SkeletalMesh->GetMaterials())
	{
		OutSurfacesMaterial.Add(Material.MaterialInterface);
	}

	return true;
}
