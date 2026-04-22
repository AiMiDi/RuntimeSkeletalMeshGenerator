#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "RuntimeSkeletalMeshGenerator.h"

#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshRenderData.h"

namespace
{
USkeleton* CreateTestSkeleton()
{
	USkeleton* Skeleton = NewObject<USkeleton>(GetTransientPackage(), NAME_None, RF_Transient);
	FReferenceSkeletonModifier ReferenceSkeletonModifier(Skeleton);
	ReferenceSkeletonModifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
	ReferenceSkeletonModifier.Add(FMeshBoneInfo(TEXT("child"), TEXT("child"), 0), FTransform(FVector(10.0, 0.0, 0.0)));
	return Skeleton;
}

USkeletalMesh* CreateTestMesh(USkeleton* Skeleton)
{
	USkeletalMesh* SkeletalMesh = NewObject<USkeletalMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	SkeletalMesh->SetSkeleton(Skeleton);
	SkeletalMesh->SetRefSkeleton(Skeleton->GetReferenceSkeleton());
	Skeleton->MergeAllBonesToBoneTree(SkeletalMesh);
	return SkeletalMesh;
}

TArray<UMaterialInterface*> CreateMaterialSlots(const int32 Count)
{
	TArray<UMaterialInterface*> Materials;
	Materials.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Materials.Add(UMaterial::GetDefaultMaterial(MD_Surface));
	}
	return Materials;
}

FMeshSurface MakeTriangleSurface(const int32 MaterialIndex, const FVector& Offset, const int32 BoneIndex)
{
	FMeshSurface Surface;
	Surface.MaterialIndex = MaterialIndex;
	Surface.Indices = {0, 1, 2};
	Surface.Vertices = {
		Offset + FVector(0.0, 0.0, 0.0),
		Offset + FVector(10.0, 0.0, 0.0),
		Offset + FVector(0.0, 10.0, 0.0),
	};
	Surface.Tangents = {
		FVector::ForwardVector,
		FVector::ForwardVector,
		FVector::ForwardVector,
	};
	Surface.Normals = {
		FVector::UpVector,
		FVector::UpVector,
		FVector::UpVector,
	};
	Surface.FlipBinormalSigns = {false, false, false};
	Surface.Uvs = {
		{FVector2D(0.0, 0.0)},
		{FVector2D(1.0, 0.0)},
		{FVector2D(0.0, 1.0)},
	};
	Surface.Colors = {
		FColor::Red,
		FColor::Green,
		FColor::Blue,
	};
	Surface.BoneInfluences = {
		{FRawBoneInfluence(0, BoneIndex, 1.0f)},
		{FRawBoneInfluence(1, BoneIndex, 1.0f)},
		{FRawBoneInfluence(2, BoneIndex, 1.0f)},
	};
	return Surface;
}
} // namespace

BEGIN_DEFINE_SPEC(
	FRuntimeSkeletalMeshGeneratorMeshSpec,
	"RuntimeSkeletalMeshGenerator.Unit.Mesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FRuntimeSkeletalMeshGeneratorMeshSpec)

void FRuntimeSkeletalMeshGeneratorMeshSpec::Define()
{
	Describe("GenerateSkeletalMesh", [this]()
	{
		It("rejects invalid input without asserting", [this]()
		{
			USkeleton* Skeleton = CreateTestSkeleton();
			USkeletalMesh* SkeletalMesh = CreateTestMesh(Skeleton);
			const TArray<UMaterialInterface*> Materials = CreateMaterialSlots(1);

			TArray<FMeshSurface> EmptySurfaces;
			TestFalse(
				TEXT("Empty surfaces are rejected"),
				FRuntimeSkeletalMeshGenerator::GenerateSkeletalMesh(
					SkeletalMesh,
					EmptySurfaces,
					Materials));

			FMeshSurface InvalidSurface = MakeTriangleSurface(0, FVector::ZeroVector, 0);
			InvalidSurface.Normals.Pop();
			TestFalse(
				TEXT("Mismatched vertex data is rejected"),
				FRuntimeSkeletalMeshGenerator::GenerateSkeletalMesh(
					SkeletalMesh,
					{InvalidSurface},
					Materials));

			FMeshSurface InvalidBoneSurface = MakeTriangleSurface(0, FVector(20.0, 0.0, 0.0), 0);
			InvalidBoneSurface.BoneInfluences[1][0].BoneIndex = 99;
			TestFalse(
				TEXT("Invalid bone indices are rejected"),
				FRuntimeSkeletalMeshGenerator::GenerateSkeletalMesh(
					SkeletalMesh,
					{InvalidBoneSurface},
					Materials));
		});

		It("clears stale state when rebuilding the same skeletal mesh", [this]()
		{
			USkeleton* Skeleton = CreateTestSkeleton();
			USkeletalMesh* SkeletalMesh = CreateTestMesh(Skeleton);

			const TArray<UMaterialInterface*> FirstMaterials = CreateMaterialSlots(3);
			const TArray<FMeshSurface> FirstSurfaces = {
				MakeTriangleSurface(1, FVector::ZeroVector, 0),
				MakeTriangleSurface(2, FVector(20.0, 0.0, 0.0), 1),
			};

			TestTrue(
				TEXT("First build succeeds"),
				FRuntimeSkeletalMeshGenerator::GenerateSkeletalMesh(
					SkeletalMesh,
					FirstSurfaces,
					FirstMaterials));

			const TArray<UMaterialInterface*> SecondMaterials = CreateMaterialSlots(2);
			const TArray<FMeshSurface> SecondSurfaces = {
				MakeTriangleSurface(1, FVector(40.0, 0.0, 0.0), 1),
			};

			TestTrue(
				TEXT("Second build succeeds"),
				FRuntimeSkeletalMeshGenerator::GenerateSkeletalMesh(
					SkeletalMesh,
					SecondSurfaces,
					SecondMaterials));

			const FSkeletalMeshRenderData* RenderData = SkeletalMesh->GetResourceForRendering();
			TestNotNull(TEXT("Render data exists"), RenderData);
			TestEqual(TEXT("Material slots are rebuilt"), SkeletalMesh->GetMaterials().Num(), 2);
			TestEqual(TEXT("LOD section count reflects the second build"), RenderData->LODRenderData[0].RenderSections.Num(), 1);

			TArray<uint32> IndexBuffer;
			RenderData->LODRenderData[0].MultiSizeIndexContainer.GetIndexBuffer(IndexBuffer);
			TestEqual(TEXT("Index buffer only contains the second surface"), IndexBuffer.Num(), 3);
		});
	});

	Describe("DecomposeSkeletalMesh", [this]()
	{
		It("preserves material indices across decompose and regenerate", [this]()
		{
			USkeleton* Skeleton = CreateTestSkeleton();
			USkeletalMesh* FirstMesh = CreateTestMesh(Skeleton);
			const TArray<UMaterialInterface*> Materials = CreateMaterialSlots(3);
			const TArray<FMeshSurface> Surfaces = {
				MakeTriangleSurface(2, FVector::ZeroVector, 0),
				MakeTriangleSurface(0, FVector(20.0, 0.0, 0.0), 1),
			};

			TestTrue(
				TEXT("Initial mesh build succeeds"),
				FRuntimeSkeletalMeshGenerator::GenerateSkeletalMesh(
					FirstMesh,
					Surfaces,
					Materials));

			TArray<FMeshSurface> DecomposedSurfaces;
			TArray<int32> VertexOffsets;
			TArray<int32> IndexOffsets;
			TArray<UMaterialInterface*> DecomposedMaterials;
			TestTrue(
				TEXT("Mesh decompose succeeds"),
				FRuntimeSkeletalMeshGenerator::DecomposeSkeletalMesh(
					FirstMesh,
					DecomposedSurfaces,
					VertexOffsets,
					IndexOffsets,
					DecomposedMaterials));

			TestEqual(TEXT("Material slot count is preserved"), DecomposedMaterials.Num(), 3);
			TestEqual(TEXT("First surface material index is preserved"), DecomposedSurfaces[0].MaterialIndex, 2);
			TestEqual(TEXT("Second surface material index is preserved"), DecomposedSurfaces[1].MaterialIndex, 0);

			USkeletalMesh* SecondMesh = CreateTestMesh(Skeleton);
			TestTrue(
				TEXT("Regenerated mesh build succeeds"),
				FRuntimeSkeletalMeshGenerator::GenerateSkeletalMesh(
					SecondMesh,
					DecomposedSurfaces,
					DecomposedMaterials));

			TArray<FMeshSurface> RoundTripSurfaces;
			TArray<int32> RoundTripVertexOffsets;
			TArray<int32> RoundTripIndexOffsets;
			TArray<UMaterialInterface*> RoundTripMaterials;
			TestTrue(
				TEXT("Round-trip decompose succeeds"),
				FRuntimeSkeletalMeshGenerator::DecomposeSkeletalMesh(
					SecondMesh,
					RoundTripSurfaces,
					RoundTripVertexOffsets,
					RoundTripIndexOffsets,
					RoundTripMaterials));

			TestEqual(TEXT("Round-trip first material index matches"), RoundTripSurfaces[0].MaterialIndex, 2);
			TestEqual(TEXT("Round-trip second material index matches"), RoundTripSurfaces[1].MaterialIndex, 0);
		});
	});
}

#endif
