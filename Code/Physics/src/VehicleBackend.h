#pragma once

// Unit terjemahan kedua — dan satu-satunya yang lain — yang melihat PhysX.
//
// Dipisahkan dari `PhysicsWorld.cpp` bukan karena batasnya berbeda melainkan
// karena `PxVehicle` menuntut sekitar sebelas antarmuka komponen diimplementasi
// sekaligus; menaruhnya di berkas yang sama akan mengubur lifecycle scene di
// bawah seribu baris papan ketik kendaraan. Aturannya tetap: uji penyisir di
// `SimPhysicsTests` menyebut daftar berkas yang boleh melihat PhysX secara
// eksplisit, dan daftar itu sengaja pendek.

#if SIM_WITH_PHYSX

#include "Sim/Physics/PhysicsVehicle.h"

#include <PxPhysicsAPI.h>
#include <vehicle2/PxVehicleAPI.h>

#include <string>

namespace sim::physics {

/// Kerangka sumbu kendaraan untuk dunia ini: maju +Z, samping +X, atas **+Y**.
///
/// **Bawaan PhysX adalah Z-atas, dan mesin ini Y-atas.** Selisih itu tidak
/// pernah muncul sebagai galat: suspensi menembakkan sinarnya mendatar,
/// tidak menemukan apa-apa selamanya, dan mobil duduk di atas bak chassis-nya
/// dengan roda menggantung — gejala yang terbaca sebagai "kendaraan tidak
/// jalan" dan menunjuk ke mana-mana kecuali ke sumbu.
///
/// Satu fungsi, dipakai backend **dan** konteks simulasi dunia. Dua tempat yang
/// menyusun kerangka sendiri-sendiri adalah dua tempat yang bisa berbeda.
physx::vehicle2::PxVehicleFrame SimVehicleFrame();

/// Satu kendaraan direct-drive, beserta seluruh parameter dan keadaannya.
///
/// Mengimplementasi antarmuka komponen `PxVehicle` secara langsung, mengikuti
/// rancangan yang dipakai snippet resmi: komponen tidak menyimpan data sendiri,
/// ia menanyakannya lewat `getDataFor...`. Satu kelas yang menjawab semuanya
/// lebih pendek daripada sebelas kelas yang masing-masing memegang rujukan ke
/// data yang sama.
class VehicleInstance final
    : public physx::vehicle2::PxVehiclePhysXActorBeginComponent,
      public physx::vehicle2::PxVehiclePhysXConstraintComponent,
      public physx::vehicle2::PxVehiclePhysXRoadGeometrySceneQueryComponent,
      public physx::vehicle2::PxVehiclePhysXActorEndComponent,
      public physx::vehicle2::PxVehicleRigidBodyComponent,
      public physx::vehicle2::PxVehicleSuspensionComponent,
      public physx::vehicle2::PxVehicleTireComponent,
      public physx::vehicle2::PxVehicleWheelComponent,
      public physx::vehicle2::PxVehicleDirectDriveCommandResponseComponent,
      public physx::vehicle2::PxVehicleDirectDriveActuationStateComponent,
      public physx::vehicle2::PxVehicleDirectDrivetrainComponent,
      public physx::vehicle2::PxVehicleEngineDriveCommandResponseComponent,
      public physx::vehicle2::PxVehicleMultiWheelDriveDifferentialStateComponent,
      public physx::vehicle2::PxVehicleEngineDriveActuationStateComponent,
      public physx::vehicle2::PxVehicleEngineDrivetrainComponent {
public:
    /// Membangun dari deskriptor. False bila deskriptornya tidak masuk akal;
    /// `error` menyebut yang mana.
    bool Create(const VehicleDesc& desc, physx::PxPhysics& physics,
                const physx::PxCookingParams& cooking, physx::PxMaterial& material,
                std::string& error);
    void Destroy();

    void AddToScene(physx::PxScene& scene);
    void RemoveFromScene(physx::PxScene& scene);

    void SetInput(const VehicleInput& input);
    void Step(float dt, const physx::vehicle2::PxVehicleSimulationContext& context);
    void ReadState(VehicleState& out) const;

    physx::PxRigidBody* RigidBody() const { return physxActor_.rigidBody; }

    // --- antarmuka komponen ---------------------------------------------------

    void getDataForRigidBodyComponent(
        const physx::vehicle2::PxVehicleAxleDescription*& axleDescription,
        const physx::vehicle2::PxVehicleRigidBodyParams*& rigidBodyParams,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleSuspensionForce>&
            suspensionForces,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleTireForce>& tireForces,
        const physx::vehicle2::PxVehicleAntiRollTorque*& antiRollTorque,
        physx::vehicle2::PxVehicleRigidBodyState*& rigidBodyState) override;

    void getDataForSuspensionComponent(
        const physx::vehicle2::PxVehicleAxleDescription*& axleDescription,
        const physx::vehicle2::PxVehicleRigidBodyParams*& rigidBodyParams,
        const physx::vehicle2::PxVehicleSuspensionStateCalculationParams*&
            suspensionStateCalculationParams,
        physx::vehicle2::PxVehicleArrayData<const physx::PxReal>& steerResponseStates,
        const physx::vehicle2::PxVehicleRigidBodyState*& rigidBodyState,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleWheelParams>&
            wheelParams,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleSuspensionParams>&
            suspensionParams,
        physx::vehicle2::PxVehicleArrayData<
            const physx::vehicle2::PxVehicleSuspensionComplianceParams>& suspensionComplianceParams,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleSuspensionForceParams>&
            suspensionForceParams,
        physx::vehicle2::PxVehicleSizedArrayData<
            const physx::vehicle2::PxVehicleAntiRollForceParams>& antiRollForceParams,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleRoadGeometryState>&
            wheelRoadGeomStates,
        physx::vehicle2::PxVehicleArrayData<physx::vehicle2::PxVehicleSuspensionState>&
            suspensionStates,
        physx::vehicle2::PxVehicleArrayData<physx::vehicle2::PxVehicleSuspensionComplianceState>&
            suspensionComplianceStates,
        physx::vehicle2::PxVehicleArrayData<physx::vehicle2::PxVehicleSuspensionForce>&
            suspensionForces,
        physx::vehicle2::PxVehicleAntiRollTorque*& antiRollTorque) override;

    void getDataForTireComponent(
        const physx::vehicle2::PxVehicleAxleDescription*& axleDescription,
        physx::vehicle2::PxVehicleArrayData<const physx::PxReal>& steerResponseStates,
        const physx::vehicle2::PxVehicleRigidBodyState*& rigidBodyState,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleWheelActuationState>&
            actuationStates,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleWheelParams>&
            wheelParams,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleSuspensionParams>&
            suspensionParams,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleTireForceParams>&
            tireForceParams,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleRoadGeometryState>&
            roadGeomStates,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleSuspensionState>&
            suspensionStates,
        physx::vehicle2::PxVehicleArrayData<
            const physx::vehicle2::PxVehicleSuspensionComplianceState>& suspensionComplianceStates,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleSuspensionForce>&
            suspensionForces,
        physx::vehicle2::PxVehicleArrayData<
            const physx::vehicle2::PxVehicleWheelRigidBody1dState>& wheelRigidBody1DStates,
        physx::vehicle2::PxVehicleArrayData<physx::vehicle2::PxVehicleTireGripState>&
            tireGripStates,
        physx::vehicle2::PxVehicleArrayData<physx::vehicle2::PxVehicleTireDirectionState>&
            tireDirectionStates,
        physx::vehicle2::PxVehicleArrayData<physx::vehicle2::PxVehicleTireSpeedState>&
            tireSpeedStates,
        physx::vehicle2::PxVehicleArrayData<physx::vehicle2::PxVehicleTireSlipState>&
            tireSlipStates,
        physx::vehicle2::PxVehicleArrayData<physx::vehicle2::PxVehicleTireCamberAngleState>&
            tireCamberAngleStates,
        physx::vehicle2::PxVehicleArrayData<physx::vehicle2::PxVehicleTireStickyState>&
            tireStickyStates,
        physx::vehicle2::PxVehicleArrayData<physx::vehicle2::PxVehicleTireForce>& tireForces)
        override;

    void getDataForWheelComponent(
        const physx::vehicle2::PxVehicleAxleDescription*& axleDescription,
        physx::vehicle2::PxVehicleArrayData<const physx::PxReal>& steerResponseStates,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleWheelParams>&
            wheelParams,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleSuspensionParams>&
            suspensionParams,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleWheelActuationState>&
            actuationStates,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleSuspensionState>&
            suspensionStates,
        physx::vehicle2::PxVehicleArrayData<
            const physx::vehicle2::PxVehicleSuspensionComplianceState>& suspensionComplianceStates,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleTireSpeedState>&
            tireSpeedStates,
        physx::vehicle2::PxVehicleArrayData<physx::vehicle2::PxVehicleWheelRigidBody1dState>&
            wheelRigidBody1dStates,
        physx::vehicle2::PxVehicleArrayData<physx::vehicle2::PxVehicleWheelLocalPose>&
            wheelLocalPoses) override;

    void getDataForPhysXActorBeginComponent(
        const physx::vehicle2::PxVehicleAxleDescription*& axleDescription,
        const physx::vehicle2::PxVehicleCommandState*& commands,
        const physx::vehicle2::PxVehicleEngineDriveTransmissionCommandState*& transmissionCommands,
        const physx::vehicle2::PxVehicleGearboxParams*& gearParams,
        const physx::vehicle2::PxVehicleGearboxState*& gearState,
        const physx::vehicle2::PxVehicleEngineParams*& engineParams,
        physx::vehicle2::PxVehiclePhysXActor*& physxActor,
        physx::vehicle2::PxVehiclePhysXSteerState*& physxSteerState,
        physx::vehicle2::PxVehiclePhysXConstraints*& physxConstraints,
        physx::vehicle2::PxVehicleRigidBodyState*& rigidBodyState,
        physx::vehicle2::PxVehicleArrayData<physx::vehicle2::PxVehicleWheelRigidBody1dState>&
            wheelRigidBody1dStates,
        physx::vehicle2::PxVehicleEngineState*& engineState) override;

    void getDataForPhysXActorEndComponent(
        const physx::vehicle2::PxVehicleAxleDescription*& axleDescription,
        const physx::vehicle2::PxVehicleRigidBodyState*& rigidBodyState,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleWheelParams>&
            wheelParams,
        physx::vehicle2::PxVehicleArrayData<const physx::PxTransform>& wheelShapeLocalPoses,
        physx::vehicle2::PxVehicleArrayData<
            const physx::vehicle2::PxVehicleWheelRigidBody1dState>& wheelRigidBody1dStates,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleWheelLocalPose>&
            wheelLocalPoses,
        const physx::vehicle2::PxVehicleGearboxState*& gearState, const physx::PxReal*& throttle,
        physx::vehicle2::PxVehiclePhysXActor*& physxActor) override;

    void getDataForPhysXConstraintComponent(
        const physx::vehicle2::PxVehicleAxleDescription*& axleDescription,
        const physx::vehicle2::PxVehicleRigidBodyState*& rigidBodyState,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleSuspensionParams>&
            suspensionParams,
        physx::vehicle2::PxVehicleArrayData<
            const physx::vehicle2::PxVehiclePhysXSuspensionLimitConstraintParams>&
            suspensionLimitParams,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleSuspensionState>&
            suspensionStates,
        physx::vehicle2::PxVehicleArrayData<
            const physx::vehicle2::PxVehicleSuspensionComplianceState>& suspensionComplianceStates,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleRoadGeometryState>&
            wheelRoadGeomStates,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleTireDirectionState>&
            tireDirectionStates,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleTireStickyState>&
            tireStickyStates,
        physx::vehicle2::PxVehiclePhysXConstraints*& constraints) override;

    void getDataForPhysXRoadGeometrySceneQueryComponent(
        const physx::vehicle2::PxVehicleAxleDescription*& axleDescription,
        const physx::vehicle2::PxVehiclePhysXRoadGeometryQueryParams*& roadGeomParams,
        physx::vehicle2::PxVehicleArrayData<const physx::PxReal>& steerResponseStates,
        const physx::vehicle2::PxVehicleRigidBodyState*& rigidBodyState,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleWheelParams>&
            wheelParams,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleSuspensionParams>&
            suspensionParams,
        physx::vehicle2::PxVehicleArrayData<
            const physx::vehicle2::PxVehiclePhysXMaterialFrictionParams>& materialFrictionParams,
        physx::vehicle2::PxVehicleArrayData<physx::vehicle2::PxVehicleRoadGeometryState>&
            roadGeometryStates,
        physx::vehicle2::PxVehicleArrayData<
            physx::vehicle2::PxVehiclePhysXRoadGeometryQueryState>& physxRoadGeometryStates)
        override;

    void getDataForDirectDriveCommandResponseComponent(
        const physx::vehicle2::PxVehicleAxleDescription*& axleDescription,
        physx::vehicle2::PxVehicleSizedArrayData<
            const physx::vehicle2::PxVehicleBrakeCommandResponseParams>& brakeResponseParams,
        const physx::vehicle2::PxVehicleDirectDriveThrottleCommandResponseParams*&
            throttleResponseParams,
        const physx::vehicle2::PxVehicleSteerCommandResponseParams*& steerResponseParams,
        physx::vehicle2::PxVehicleSizedArrayData<const physx::vehicle2::PxVehicleAckermannParams>&
            ackermannParams,
        const physx::vehicle2::PxVehicleCommandState*& commands,
        const physx::vehicle2::PxVehicleDirectDriveTransmissionCommandState*& transmissionCommands,
        const physx::vehicle2::PxVehicleRigidBodyState*& rigidBodyState,
        physx::vehicle2::PxVehicleArrayData<physx::PxReal>& brakeResponseStates,
        physx::vehicle2::PxVehicleArrayData<physx::PxReal>& throttleResponseStates,
        physx::vehicle2::PxVehicleArrayData<physx::PxReal>& steerResponseStates) override;

    void getDataForDirectDriveActuationStateComponent(
        const physx::vehicle2::PxVehicleAxleDescription*& axleDescription,
        physx::vehicle2::PxVehicleArrayData<const physx::PxReal>& brakeResponseStates,
        physx::vehicle2::PxVehicleArrayData<const physx::PxReal>& throttleResponseStates,
        physx::vehicle2::PxVehicleArrayData<physx::vehicle2::PxVehicleWheelActuationState>&
            actuationStates) override;

    void getDataForDirectDrivetrainComponent(
        const physx::vehicle2::PxVehicleAxleDescription*& axleDescription,
        physx::vehicle2::PxVehicleArrayData<const physx::PxReal>& brakeResponseStates,
        physx::vehicle2::PxVehicleArrayData<const physx::PxReal>& throttleResponseStates,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleWheelParams>&
            wheelParams,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleWheelActuationState>&
            actuationStates,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleTireForce>& tireForces,
        physx::vehicle2::PxVehicleArrayData<physx::vehicle2::PxVehicleWheelRigidBody1dState>&
            wheelRigidBody1dStates) override;

    void getDataForEngineDriveCommandResponseComponent(
        const physx::vehicle2::PxVehicleAxleDescription*& axleDescription,
        physx::vehicle2::PxVehicleSizedArrayData<
            const physx::vehicle2::PxVehicleBrakeCommandResponseParams>& brakeResponseParams,
        const physx::vehicle2::PxVehicleSteerCommandResponseParams*& steerResponseParams,
        physx::vehicle2::PxVehicleSizedArrayData<const physx::vehicle2::PxVehicleAckermannParams>&
            ackermannParams,
        const physx::vehicle2::PxVehicleGearboxParams*& gearboxParams,
        const physx::vehicle2::PxVehicleClutchCommandResponseParams*& clutchResponseParams,
        const physx::vehicle2::PxVehicleEngineParams*& engineParams,
        const physx::vehicle2::PxVehicleRigidBodyState*& rigidBodyState,
        const physx::vehicle2::PxVehicleEngineState*& engineState,
        const physx::vehicle2::PxVehicleAutoboxParams*& autoboxParams,
        const physx::vehicle2::PxVehicleCommandState*& commands,
        const physx::vehicle2::PxVehicleEngineDriveTransmissionCommandState*& transmissionCommands,
        physx::vehicle2::PxVehicleArrayData<physx::PxReal>& brakeResponseStates,
        physx::vehicle2::PxVehicleEngineDriveThrottleCommandResponseState*& throttleResponseState,
        physx::vehicle2::PxVehicleArrayData<physx::PxReal>& steerResponseStates,
        physx::vehicle2::PxVehicleGearboxState*& gearboxResponseState,
        physx::vehicle2::PxVehicleClutchCommandResponseState*& clutchResponseState,
        physx::vehicle2::PxVehicleAutoboxState*& autoboxState) override;

    void getDataForMultiWheelDriveDifferentialStateComponent(
        const physx::vehicle2::PxVehicleAxleDescription*& axleDescription,
        const physx::vehicle2::PxVehicleMultiWheelDriveDifferentialParams*& differentialParams,
        physx::vehicle2::PxVehicleDifferentialState*& differentialState) override;

    void getDataForEngineDriveActuationStateComponent(
        const physx::vehicle2::PxVehicleAxleDescription*& axleDescription,
        const physx::vehicle2::PxVehicleGearboxParams*& gearboxParams,
        physx::vehicle2::PxVehicleArrayData<const physx::PxReal>& brakeResponseStates,
        const physx::vehicle2::PxVehicleEngineDriveThrottleCommandResponseState*&
            throttleResponseState,
        const physx::vehicle2::PxVehicleGearboxState*& gearboxState,
        const physx::vehicle2::PxVehicleDifferentialState*& differentialState,
        const physx::vehicle2::PxVehicleClutchCommandResponseState*& clutchResponseState,
        physx::vehicle2::PxVehicleArrayData<physx::vehicle2::PxVehicleWheelActuationState>&
            actuationStates) override;

    void getDataForEngineDrivetrainComponent(
        const physx::vehicle2::PxVehicleAxleDescription*& axleDescription,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleWheelParams>&
            wheelParams,
        const physx::vehicle2::PxVehicleEngineParams*& engineParams,
        const physx::vehicle2::PxVehicleClutchParams*& clutchParams,
        const physx::vehicle2::PxVehicleGearboxParams*& gearboxParams,
        physx::vehicle2::PxVehicleArrayData<const physx::PxReal>& brakeResponseStates,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleWheelActuationState>&
            actuationStates,
        physx::vehicle2::PxVehicleArrayData<const physx::vehicle2::PxVehicleTireForce>& tireForces,
        const physx::vehicle2::PxVehicleEngineDriveThrottleCommandResponseState*&
            throttleResponseState,
        const physx::vehicle2::PxVehicleClutchCommandResponseState*& clutchResponseState,
        const physx::vehicle2::PxVehicleDifferentialState*& differentialState,
        const physx::vehicle2::PxVehicleWheelConstraintGroupState*& constraintGroupState,
        physx::vehicle2::PxVehicleArrayData<physx::vehicle2::PxVehicleWheelRigidBody1dState>&
            wheelRigidBody1dStates,
        physx::vehicle2::PxVehicleEngineState*& engineState,
        physx::vehicle2::PxVehicleGearboxState*& gearboxState,
        physx::vehicle2::PxVehicleClutchSlipState*& clutchState) override;

private:
    void BuildComponentSequence();

    static constexpr physx::PxU32 kMaxWheels = physx::vehicle2::PxVehicleLimits::eMAX_NB_WHEELS;

    // --- parameter ---
    physx::vehicle2::PxVehicleAxleDescription axleDescription_;
    physx::vehicle2::PxVehicleFrame frame_;
    physx::vehicle2::PxVehicleScale scale_;
    physx::vehicle2::PxVehicleSuspensionStateCalculationParams suspensionCalcParams_;
    physx::vehicle2::PxVehicleBrakeCommandResponseParams brakeParams_[2];
    physx::vehicle2::PxVehicleSteerCommandResponseParams steerParams_;
    physx::vehicle2::PxVehicleAckermannParams ackermannParams_[1];
    physx::vehicle2::PxVehicleSuspensionParams suspensionParams_[kMaxWheels];
    physx::vehicle2::PxVehicleSuspensionComplianceParams complianceParams_[kMaxWheels];
    physx::vehicle2::PxVehicleSuspensionForceParams suspensionForceParams_[kMaxWheels];
    physx::vehicle2::PxVehicleTireForceParams tireForceParams_[kMaxWheels];
    physx::vehicle2::PxVehicleWheelParams wheelParams_[kMaxWheels];
    physx::vehicle2::PxVehicleRigidBodyParams rigidBodyParams_;
    physx::vehicle2::PxVehicleDirectDriveThrottleCommandResponseParams throttleParams_;

    physx::vehicle2::PxVehiclePhysXRoadGeometryQueryParams roadGeometryParams_;
    physx::vehicle2::PxVehiclePhysXMaterialFrictionParams materialFrictionParams_[kMaxWheels];
    physx::vehicle2::PxVehiclePhysXSuspensionLimitConstraintParams
        suspensionLimitParams_[kMaxWheels];
    physx::PxTransform wheelShapeLocalPoses_[kMaxWheels];

    // --- keadaan ---
    physx::PxReal brakeResponseStates_[kMaxWheels];
    physx::PxReal steerResponseStates_[kMaxWheels];
    physx::PxReal throttleResponseStates_[kMaxWheels];
    physx::vehicle2::PxVehicleWheelActuationState actuationStates_[kMaxWheels];
    physx::vehicle2::PxVehicleRoadGeometryState roadGeomStates_[kMaxWheels];
    physx::vehicle2::PxVehicleSuspensionState suspensionStates_[kMaxWheels];
    physx::vehicle2::PxVehicleSuspensionComplianceState complianceStates_[kMaxWheels];
    physx::vehicle2::PxVehicleSuspensionForce suspensionForces_[kMaxWheels];
    physx::vehicle2::PxVehicleTireGripState tireGripStates_[kMaxWheels];
    physx::vehicle2::PxVehicleTireDirectionState tireDirectionStates_[kMaxWheels];
    physx::vehicle2::PxVehicleTireSpeedState tireSpeedStates_[kMaxWheels];
    physx::vehicle2::PxVehicleTireSlipState tireSlipStates_[kMaxWheels];
    physx::vehicle2::PxVehicleTireCamberAngleState tireCamberStates_[kMaxWheels];
    physx::vehicle2::PxVehicleTireStickyState tireStickyStates_[kMaxWheels];
    physx::vehicle2::PxVehicleTireForce tireForces_[kMaxWheels];
    physx::vehicle2::PxVehicleWheelRigidBody1dState wheel1dStates_[kMaxWheels];
    physx::vehicle2::PxVehicleWheelLocalPose wheelLocalPoses_[kMaxWheels];
    physx::vehicle2::PxVehicleRigidBodyState rigidBodyState_;

    physx::vehicle2::PxVehicleCommandState commandState_;
    physx::vehicle2::PxVehicleDirectDriveTransmissionCommandState transmissionState_;

    // --- mesin, kopling, girboks: dipakai hanya pada EngineDrive ---
    VehicleDriveModel driveModel_ = VehicleDriveModel::DirectDrive;
    physx::vehicle2::PxVehicleEngineParams engineParams_;
    physx::vehicle2::PxVehicleClutchParams clutchParams_;
    physx::vehicle2::PxVehicleClutchCommandResponseParams clutchResponseParams_;
    physx::vehicle2::PxVehicleGearboxParams gearboxParams_;
    physx::vehicle2::PxVehicleAutoboxParams autoboxParams_;
    physx::vehicle2::PxVehicleMultiWheelDriveDifferentialParams differentialParams_;

    physx::vehicle2::PxVehicleEngineState engineState_;
    physx::vehicle2::PxVehicleGearboxState gearboxState_;
    physx::vehicle2::PxVehicleClutchCommandResponseState clutchResponseState_;
    physx::vehicle2::PxVehicleAutoboxState autoboxState_;
    physx::vehicle2::PxVehicleDifferentialState differentialState_;
    physx::vehicle2::PxVehicleClutchSlipState clutchSlipState_;
    physx::vehicle2::PxVehicleEngineDriveThrottleCommandResponseState engineThrottleState_;
    physx::vehicle2::PxVehicleEngineDriveTransmissionCommandState engineTransmission_;

    physx::vehicle2::PxVehiclePhysXActor physxActor_;
    physx::vehicle2::PxVehiclePhysXSteerState physxSteerState_;
    physx::vehicle2::PxVehiclePhysXConstraints physxConstraints_;

    physx::vehicle2::PxVehicleComponentSequence sequence_;
    physx::PxU8 substepGroup_ = 0;

    physx::PxU32 wheelCount_ = 0;
    bool inScene_ = false;
};

}  // namespace sim::physics

#endif  // SIM_WITH_PHYSX
