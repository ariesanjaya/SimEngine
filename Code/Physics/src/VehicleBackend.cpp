#include "VehicleBackend.h"

#if SIM_WITH_PHYSX

#include <algorithm>
#include <cmath>

namespace sim::physics {

using namespace physx;
using namespace physx::vehicle2;

namespace {

PxVec3 ToPxV(const Vec3& v) { return PxVec3(v.x, v.y, v.z); }
Vec3 FromPxV(const PxVec3& v) { return Vec3(v.x, v.y, v.z); }
PxQuat ToPxQ(const Quat& q) { return PxQuat(q.x, q.y, q.z, q.w); }
Quat FromPxQ(const PxQuat& q) { return Quat(q.w, q.x, q.y, q.z); }

}  // namespace

PxVehicleFrame SimVehicleFrame() {
    PxVehicleFrame frame;
    frame.lngAxis = PxVehicleAxes::ePosZ;
    frame.latAxis = PxVehicleAxes::ePosX;
    frame.vrtAxis = PxVehicleAxes::ePosY;
    return frame;
}

bool VehicleInstance::Create(const VehicleDesc& desc, PxPhysics& physics,
                             const PxCookingParams& cooking, PxMaterial& material,
                             std::string& error) {
    if (desc.wheels.size() < 2 || desc.wheels.size() > kMaxWheels) {
        error = "a vehicle needs between 2 and " + std::to_string(kMaxWheels) + " wheels";
        return false;
    }
    if (!(desc.chassisMass > 0.0f)) {
        error = "vehicle chassis mass must be positive";
        return false;
    }

    wheelCount_ = static_cast<PxU32>(desc.wheels.size());

    frame_ = SimVehicleFrame();
    scale_.setToDefault();
    // **Raycast, bukan sweep.** Sapuan menuntut mesh silinder satuan yang
    // disiapkan terpisah, dan bila mesh itu gagal dibuat sapuannya tidak
    // menemukan apa pun **tanpa satu pun pesan** — roda menggantung di udara dan
    // mobil hanya diam. Raycast tidak butuh mesh sama sekali. Sapuan memang
    // lebih benar untuk roda lebar di tepi trotoar, dan itu setelan yang bisa
    // dinaikkan nanti; yang lebih dulu harus benar adalah rantai dasarnya.
    suspensionCalcParams_.suspensionJounceCalculationType =
        PxVehicleSuspensionJounceCalculationType::eRAYCAST;
    suspensionCalcParams_.limitSuspensionExpansionVelocity = false;

    // **Sumbu disusun dari posisi roda, bukan dari urutannya.** Yang di depan
    // adalah yang koordinat memanjangnya lebih besar; menyerahkannya kepada
    // urutan dalam `desc.wheels` berarti menukar dua baris di pemanggil
    // membalik mobilnya tanpa satu pun pesan.
    const PxVec3 up = frame_.getVrtAxis();
    const PxVec3 forward = frame_.getLngAxis();

    std::vector<PxU32> front;
    std::vector<PxU32> rear;
    float midpoint = 0.0f;
    for (const VehicleWheelDesc& wheel : desc.wheels) {
        midpoint += ToPxV(wheel.centerOffset).dot(forward);
    }
    midpoint /= static_cast<float>(wheelCount_);
    for (PxU32 i = 0; i < wheelCount_; ++i) {
        if (ToPxV(desc.wheels[i].centerOffset).dot(forward) >= midpoint) {
            front.push_back(i);
        } else {
            rear.push_back(i);
        }
    }

    axleDescription_.setToDefault();
    if (!front.empty()) {
        axleDescription_.addAxle(static_cast<PxU32>(front.size()), front.data());
    }
    if (!rear.empty()) {
        axleDescription_.addAxle(static_cast<PxU32>(rear.size()), rear.data());
    }

    rigidBodyParams_.mass = desc.chassisMass;
    // Inersia kotak padat. Dihitung, bukan ditebak: inersia yang terlalu kecil
    // membuat mobil berputar seperti gasing di setiap sentuhan.
    const Vec3 e = desc.chassisHalfExtents * 2.0f;
    rigidBodyParams_.moi = PxVec3(desc.chassisMass * (e.y * e.y + e.z * e.z) / 12.0f,
                                  desc.chassisMass * (e.x * e.x + e.z * e.z) / 12.0f,
                                  desc.chassisMass * (e.x * e.x + e.y * e.y) / 12.0f);

    // Massa tertopang per roda, dibagi rata. PhysX punya penghitung yang
    // memperhitungkan letak titik berat; pembagian rata sudah cukup untuk
    // kendaraan simetris dan tidak menuntut urutan roda tertentu.
    const float sprungMass = desc.chassisMass / static_cast<float>(wheelCount_);

    for (PxU32 i = 0; i < wheelCount_; ++i) {
        const VehicleWheelDesc& wheel = desc.wheels[i];

        wheelParams_[i].radius = wheel.radius;
        wheelParams_[i].halfWidth = wheel.width * 0.5f;
        wheelParams_[i].mass = wheel.mass;
        wheelParams_[i].moi = wheel.momentOfInertia > 0.0f
                                  ? wheel.momentOfInertia
                                  : 0.5f * wheel.mass * wheel.radius * wheel.radius;
        wheelParams_[i].dampingRate = 0.25f;

        // `suspensionAttachment` adalah pose roda pada kompresi maksimum, dan
        // roda menjulur ke bawah sejauh `suspensionTravelDist` dari sana. Titik
        // yang diminta pemanggil adalah tengah jangkauannya.
        suspensionParams_[i].suspensionAttachment = PxTransform(
            ToPxV(wheel.centerOffset) + up * (wheel.suspensionTravel * 0.5f), PxQuat(PxIdentity));
        suspensionParams_[i].suspensionTravelDir = -up;
        suspensionParams_[i].suspensionTravelDist = wheel.suspensionTravel;
        suspensionParams_[i].wheelAttachment = PxTransform(PxIdentity);

        // Kekakuan bawaan dari frekuensi alami 1,5 Hz — sekitar sedan penumpang.
        // Diturunkan dari massa, bukan angka tetap: pegas yang benar untuk sedan
        // membuat truk duduk di bempernya.
        const float stiffness = wheel.springStrength > 0.0f
                                    ? wheel.springStrength
                                    : sprungMass * (2.0f * 3.14159265f * 1.5f) *
                                          (2.0f * 3.14159265f * 1.5f);
        // **Teredam kritis secara bawaan**, dan itu langsung menjawab kriteria
        // terima P6: suspensi yang tidak berosilasi di atas permukaan datar.
        // c = 2·sqrt(k·m) adalah batas di mana ia kembali ke posisi diam tanpa
        // melewatinya sama sekali.
        const float damping = wheel.springDamperRate > 0.0f
                                  ? wheel.springDamperRate
                                  : 2.0f * std::sqrt(stiffness * sprungMass);

        suspensionForceParams_[i].stiffness = stiffness;
        suspensionForceParams_[i].damping = damping;
        suspensionForceParams_[i].sprungMass = sprungMass;

        complianceParams_[i].wheelToeAngle.addPair(0.0f, 0.0f);
        complianceParams_[i].wheelCamberAngle.addPair(0.0f, 0.0f);
        complianceParams_[i].suspForceAppPoint.addPair(0.0f, PxVec3(0, 0, 0));
        complianceParams_[i].tireForceAppPoint.addPair(0.0f, PxVec3(0, 0, 0));

        tireForceParams_[i].latStiffX = 0.01f;
        tireForceParams_[i].latStiffY = 0.5f * 180.0f / 3.14159265f * sprungMass * 9.81f;
        tireForceParams_[i].longStiff = 5000.0f * sprungMass * 9.81f / 1000.0f;
        tireForceParams_[i].camberStiff = 0.0f;
        tireForceParams_[i].restLoad = sprungMass * 9.81f;
        tireForceParams_[i].frictionVsSlip[0][0] = 0.0f;
        tireForceParams_[i].frictionVsSlip[0][1] = 1.0f;
        tireForceParams_[i].frictionVsSlip[1][0] = 0.1f;
        tireForceParams_[i].frictionVsSlip[1][1] = 1.0f;
        tireForceParams_[i].frictionVsSlip[2][0] = 1.0f;
        tireForceParams_[i].frictionVsSlip[2][1] = 1.0f;
        tireForceParams_[i].loadFilter[0][0] = 0.0f;
        tireForceParams_[i].loadFilter[0][1] = 0.23f;
        tireForceParams_[i].loadFilter[1][0] = 3.0f;
        tireForceParams_[i].loadFilter[1][1] = 3.0f;

        materialFrictionParams_[i].defaultFriction = desc.tireFriction;
        materialFrictionParams_[i].materialFrictions = nullptr;
        materialFrictionParams_[i].nbMaterialFrictions = 0;

        suspensionLimitParams_[i].restitution = 0.0f;
        suspensionLimitParams_[i].directionForSuspensionLimitConstraint =
            PxVehiclePhysXSuspensionLimitConstraintParams::eROAD_GEOMETRY_NORMAL;

        wheelShapeLocalPoses_[i] = PxTransform(PxIdentity);
    }

    // Respons perintah. Indeks 0 rem kaki, indeks 1 rem tangan.
    brakeParams_[0].maxResponse = desc.maxBrakeTorque;
    brakeParams_[1].maxResponse = desc.maxHandbrakeTorque;
    steerParams_.maxResponse = desc.maxSteerAngle;
    throttleParams_.maxResponse = desc.peakDriveTorque;
    for (PxU32 i = 0; i < wheelCount_; ++i) {
        const VehicleWheelDesc& wheel = desc.wheels[i];
        brakeParams_[0].wheelResponseMultipliers[i] = 1.0f;
        brakeParams_[1].wheelResponseMultipliers[i] = wheel.handbraked ? 1.0f : 0.0f;
        steerParams_.wheelResponseMultipliers[i] = wheel.steered ? 1.0f : 0.0f;
        throttleParams_.wheelResponseMultipliers[i] = wheel.driven ? 1.0f : 0.0f;
    }
    // Respons linear terhadap perintah, tidak bergantung laju — itulah keadaan
    // bawaan tabel nonlinear, dan disetel eksplisit karena struct ini tidak
    // punya konstruktor yang melakukannya.
    brakeParams_[0].nonlinearResponse.clear();
    brakeParams_[1].nonlinearResponse.clear();
    steerParams_.nonlinearResponse.clear();
    throttleParams_.nonlinearResponse.clear();

    // Ackermann dimatikan: ia menuntut sepasang roda kemudi yang disebutkan
    // eksplisit, dan kendaraan di sini boleh punya susunan roda apa pun.
    ackermannParams_[0].wheelIds[0] = PxVehicleLimits::eMAX_NB_WHEELS;
    ackermannParams_[0].wheelIds[1] = PxVehicleLimits::eMAX_NB_WHEELS;
    ackermannParams_[0].wheelBase = 1.0f;
    ackermannParams_[0].trackWidth = 1.0f;
    ackermannParams_[0].strength = 0.0f;

    roadGeometryParams_.roadGeometryQueryType = PxVehiclePhysXRoadGeometryQueryType::eRAYCAST;
    // **Mask-nya semua lapisan, bukan nol.** Sejak P2 setiap bentuk membawa
    // lapisannya di `word0` data filter query, dan query yang membawa nol tidak
    // cocok dengan apa pun — roda tidak menemukan tanah, mobil duduk di atas
    // bak chassis-nya, dan tidak ada satu pun pesan yang menyebut lapisan.
    // Kendaraan berjalan di atas segala sesuatu kecuali diberi tahu sebaliknya.
    roadGeometryParams_.defaultFilterData =
        PxQueryFilterData(PxFilterData(0xFFFFFFFFu, 0, 0, 0), PxQueryFlag::eSTATIC);
    roadGeometryParams_.filterCallback = nullptr;
    roadGeometryParams_.filterDataEntries = nullptr;

    // Keadaan awal nol.
    for (PxU32 i = 0; i < kMaxWheels; ++i) {
        brakeResponseStates_[i] = 0.0f;
        steerResponseStates_[i] = 0.0f;
        throttleResponseStates_[i] = 0.0f;
        actuationStates_[i].setToDefault();
        roadGeomStates_[i].setToDefault();
        suspensionStates_[i].setToDefault();
        complianceStates_[i].setToDefault();
        suspensionForces_[i].setToDefault();
        tireGripStates_[i].setToDefault();
        tireDirectionStates_[i].setToDefault();
        tireSpeedStates_[i].setToDefault();
        tireSlipStates_[i].setToDefault();
        tireCamberStates_[i].setToDefault();
        tireStickyStates_[i].setToDefault();
        tireForces_[i].setToDefault();
        wheel1dStates_[i].setToDefault();
        wheelLocalPoses_[i].setToDefault();
    }
    rigidBodyState_.setToDefault();
    commandState_.setToDefault();
    transmissionState_.setToDefault();
    transmissionState_.gear = PxVehicleDirectDriveTransmissionCommandState::eFORWARD;

    // Aktor PhysX beserta bentuk chassis dan rodanya.
    const PxVehiclePhysXRigidActorParams actorParams(rigidBodyParams_, nullptr);
    const PxBoxGeometry chassisGeometry(ToPxV(desc.chassisHalfExtents));
    // **Bendera bentuk disebut, bukan dibiarkan nol.** Helper PhysX memanggil
    // `setFlags` apa adanya, jadi `PxShapeFlags(0)` berarti benar-benar tanpa
    // `eSIMULATION_SHAPE` — chassis tanpa itu jatuh menembus lantai alih-alih
    // menabraknya, dan gejalanya tidak menyebut bendera mana pun.
    const PxShapeFlags chassisFlags =
        PxShapeFlag::eSIMULATION_SHAPE | PxShapeFlag::eSCENE_QUERY_SHAPE;
    // Roda **tidak** ikut disimulasikan: yang menahan mobil di atas tanah adalah
    // suspensinya, dan bentuk roda yang ikut menabrak akan melawan suspensi itu
    // sendiri. Ia tetap bisa di-query supaya peluru dan picking mengenainya.
    const PxShapeFlags wheelFlags = PxShapeFlag::eSCENE_QUERY_SHAPE;

    const PxVehiclePhysXRigidActorShapeParams shapeParams(
        chassisGeometry, PxTransform(PxIdentity), material, chassisFlags, PxFilterData(),
        PxFilterData());
    const PxVehiclePhysXWheelParams physxWheelParams(axleDescription_, wheelParams_);
    const PxVehiclePhysXWheelShapeParams wheelShapeParams(material, wheelFlags, PxFilterData(),
                                                          PxFilterData());

    PxVehiclePhysXActorCreate(frame_, actorParams,
                              PxTransform(ToPxV(desc.centerOfMassOffset), PxQuat(PxIdentity)),
                              shapeParams, physxWheelParams, wheelShapeParams, physics, cooking,
                              physxActor_);
    if (physxActor_.rigidBody == nullptr) {
        error = "creating the vehicle actor failed";
        return false;
    }
    physxActor_.rigidBody->setGlobalPose(PxTransform(ToPxV(desc.position), ToPxQ(desc.rotation)));

    PxVehicleConstraintsCreate(axleDescription_, physics, *physxActor_.rigidBody,
                               physxConstraints_);
    physxSteerState_.setToDefault();

    BuildComponentSequence();
    return true;
}

void VehicleInstance::BuildComponentSequence() {
    sequence_.add(static_cast<PxVehiclePhysXActorBeginComponent*>(this));
    sequence_.add(static_cast<PxVehicleDirectDriveCommandResponseComponent*>(this));
    sequence_.add(static_cast<PxVehicleDirectDriveActuationStateComponent*>(this));
    sequence_.add(static_cast<PxVehiclePhysXRoadGeometrySceneQueryComponent*>(this));

    // Suspensi, ban, dan roda dijalankan tiga kali per langkah tanpa menghitung
    // ulang bidang di bawah roda. **Bukan pemborosan melainkan yang membuatnya
    // stabil pada laju rendah**, dan jauh lebih murah daripada menjalankan
    // seluruh urutan pada langkah yang lebih kecil.
    substepGroup_ = sequence_.beginSubstepGroup(3);
    sequence_.add(static_cast<PxVehicleSuspensionComponent*>(this));
    sequence_.add(static_cast<PxVehicleTireComponent*>(this));
    sequence_.add(static_cast<PxVehiclePhysXConstraintComponent*>(this));
    sequence_.add(static_cast<PxVehicleDirectDrivetrainComponent*>(this));
    sequence_.add(static_cast<PxVehicleWheelComponent*>(this));
    sequence_.add(static_cast<PxVehicleRigidBodyComponent*>(this));
    sequence_.endSubstepGroup();

    sequence_.add(static_cast<PxVehiclePhysXActorEndComponent*>(this));
}

void VehicleInstance::Destroy() {
    PxVehicleConstraintsDestroy(physxConstraints_);
    PxVehiclePhysXActorDestroy(physxActor_);
    physxActor_.rigidBody = nullptr;
}

void VehicleInstance::AddToScene(PxScene& scene) {
    if (physxActor_.rigidBody != nullptr && !inScene_) {
        scene.addActor(*physxActor_.rigidBody);
        inScene_ = true;
    }
}

void VehicleInstance::RemoveFromScene(PxScene& scene) {
    if (physxActor_.rigidBody != nullptr && inScene_) {
        scene.removeActor(*physxActor_.rigidBody);
        inScene_ = false;
    }
}

void VehicleInstance::SetInput(const VehicleInput& input) {
    commandState_.throttle = std::clamp(input.throttle, 0.0f, 1.0f);
    commandState_.brakes[0] = std::clamp(input.brake, 0.0f, 1.0f);
    commandState_.brakes[1] = std::clamp(input.handbrake, 0.0f, 1.0f);
    commandState_.nbBrakes = 2;
    commandState_.steer = std::clamp(input.steer, -1.0f, 1.0f);
    transmissionState_.gear = input.reverse
                                  ? PxVehicleDirectDriveTransmissionCommandState::eREVERSE
                                  : PxVehicleDirectDriveTransmissionCommandState::eFORWARD;
}

void VehicleInstance::Step(float dt, const PxVehicleSimulationContext& context) {
    sequence_.setSubsteps(substepGroup_, 3);
    sequence_.update(dt, context);
}

void VehicleInstance::ReadState(VehicleState& out) const {
    out.wheels.clear();
    if (physxActor_.rigidBody == nullptr) {
        return;
    }
    const PxTransform pose = physxActor_.rigidBody->getGlobalPose();
    out.position = FromPxV(pose.p);
    out.rotation = FromPxQ(pose.q);
    out.linearVelocity = FromPxV(physxActor_.rigidBody->getLinearVelocity());
    out.forwardSpeed = rigidBodyState_.getLongitudinalSpeed(frame_);

    // **Pose roda relatif terhadap kerangka pusat massa, bukan kerangka aktor.**
    // Menyusunnya langsung dengan pose aktor menggeser seluruh roda sejauh
    // offset titik berat — di sini 0,35 m, yang persis sama dengan jari-jari
    // roda dan karena itu terbaca seolah mobil melayang setinggi satu roda.
    const PxTransform comLocal = physxActor_.rigidBody->getCMassLocalPose();
    out.wheels.reserve(wheelCount_);
    for (PxU32 i = 0; i < wheelCount_; ++i) {
        VehicleWheelState wheel;
        const PxTransform local = wheelLocalPoses_[i].localPose;
        const PxTransform world = pose * comLocal * local;
        wheel.position = FromPxV(world.p);
        wheel.rotation = FromPxQ(world.q);
        const float travel = suspensionParams_[i].suspensionTravelDist;
        wheel.suspensionCompression =
            travel > 0.0f ? std::clamp(suspensionStates_[i].jounce / travel, 0.0f, 1.0f) : 0.0f;
        wheel.rotationSpeed = wheel1dStates_[i].rotationSpeed;
        wheel.onGround = roadGeomStates_[i].hitState;
        out.wheels.push_back(wheel);
    }
}

// --- antarmuka komponen -------------------------------------------------------

void VehicleInstance::getDataForRigidBodyComponent(
    const PxVehicleAxleDescription*& axleDescription,
    const PxVehicleRigidBodyParams*& rigidBodyParams,
    PxVehicleArrayData<const PxVehicleSuspensionForce>& suspensionForces,
    PxVehicleArrayData<const PxVehicleTireForce>& tireForces,
    const PxVehicleAntiRollTorque*& antiRollTorque, PxVehicleRigidBodyState*& rigidBodyState) {
    axleDescription = &axleDescription_;
    rigidBodyParams = &rigidBodyParams_;
    suspensionForces.setData(suspensionForces_);
    tireForces.setData(tireForces_);
    antiRollTorque = nullptr;
    rigidBodyState = &rigidBodyState_;
}

void VehicleInstance::getDataForSuspensionComponent(
    const PxVehicleAxleDescription*& axleDescription,
    const PxVehicleRigidBodyParams*& rigidBodyParams,
    const PxVehicleSuspensionStateCalculationParams*& suspensionStateCalculationParams,
    PxVehicleArrayData<const PxReal>& steerResponseStates,
    const PxVehicleRigidBodyState*& rigidBodyState,
    PxVehicleArrayData<const PxVehicleWheelParams>& wheelParams,
    PxVehicleArrayData<const PxVehicleSuspensionParams>& suspensionParams,
    PxVehicleArrayData<const PxVehicleSuspensionComplianceParams>& suspensionComplianceParams,
    PxVehicleArrayData<const PxVehicleSuspensionForceParams>& suspensionForceParams,
    PxVehicleSizedArrayData<const PxVehicleAntiRollForceParams>& antiRollForceParams,
    PxVehicleArrayData<const PxVehicleRoadGeometryState>& wheelRoadGeomStates,
    PxVehicleArrayData<PxVehicleSuspensionState>& suspensionStates,
    PxVehicleArrayData<PxVehicleSuspensionComplianceState>& suspensionComplianceStates,
    PxVehicleArrayData<PxVehicleSuspensionForce>& suspensionForces,
    PxVehicleAntiRollTorque*& antiRollTorque) {
    axleDescription = &axleDescription_;
    rigidBodyParams = &rigidBodyParams_;
    suspensionStateCalculationParams = &suspensionCalcParams_;
    steerResponseStates.setData(steerResponseStates_);
    rigidBodyState = &rigidBodyState_;
    wheelParams.setData(wheelParams_);
    suspensionParams.setData(suspensionParams_);
    suspensionComplianceParams.setData(complianceParams_);
    suspensionForceParams.setData(suspensionForceParams_);
    antiRollForceParams.setEmpty();
    wheelRoadGeomStates.setData(roadGeomStates_);
    suspensionStates.setData(suspensionStates_);
    suspensionComplianceStates.setData(complianceStates_);
    suspensionForces.setData(suspensionForces_);
    antiRollTorque = nullptr;
}

void VehicleInstance::getDataForTireComponent(
    const PxVehicleAxleDescription*& axleDescription,
    PxVehicleArrayData<const PxReal>& steerResponseStates,
    const PxVehicleRigidBodyState*& rigidBodyState,
    PxVehicleArrayData<const PxVehicleWheelActuationState>& actuationStates,
    PxVehicleArrayData<const PxVehicleWheelParams>& wheelParams,
    PxVehicleArrayData<const PxVehicleSuspensionParams>& suspensionParams,
    PxVehicleArrayData<const PxVehicleTireForceParams>& tireForceParams,
    PxVehicleArrayData<const PxVehicleRoadGeometryState>& roadGeomStates,
    PxVehicleArrayData<const PxVehicleSuspensionState>& suspensionStates,
    PxVehicleArrayData<const PxVehicleSuspensionComplianceState>& suspensionComplianceStates,
    PxVehicleArrayData<const PxVehicleSuspensionForce>& suspensionForces,
    PxVehicleArrayData<const PxVehicleWheelRigidBody1dState>& wheelRigidBody1DStates,
    PxVehicleArrayData<PxVehicleTireGripState>& tireGripStates,
    PxVehicleArrayData<PxVehicleTireDirectionState>& tireDirectionStates,
    PxVehicleArrayData<PxVehicleTireSpeedState>& tireSpeedStates,
    PxVehicleArrayData<PxVehicleTireSlipState>& tireSlipStates,
    PxVehicleArrayData<PxVehicleTireCamberAngleState>& tireCamberAngleStates,
    PxVehicleArrayData<PxVehicleTireStickyState>& tireStickyStates,
    PxVehicleArrayData<PxVehicleTireForce>& tireForces) {
    axleDescription = &axleDescription_;
    steerResponseStates.setData(steerResponseStates_);
    rigidBodyState = &rigidBodyState_;
    actuationStates.setData(actuationStates_);
    wheelParams.setData(wheelParams_);
    suspensionParams.setData(suspensionParams_);
    tireForceParams.setData(tireForceParams_);
    roadGeomStates.setData(roadGeomStates_);
    suspensionStates.setData(suspensionStates_);
    suspensionComplianceStates.setData(complianceStates_);
    suspensionForces.setData(suspensionForces_);
    wheelRigidBody1DStates.setData(wheel1dStates_);
    tireGripStates.setData(tireGripStates_);
    tireDirectionStates.setData(tireDirectionStates_);
    tireSpeedStates.setData(tireSpeedStates_);
    tireSlipStates.setData(tireSlipStates_);
    tireCamberAngleStates.setData(tireCamberStates_);
    tireStickyStates.setData(tireStickyStates_);
    tireForces.setData(tireForces_);
}

void VehicleInstance::getDataForWheelComponent(
    const PxVehicleAxleDescription*& axleDescription,
    PxVehicleArrayData<const PxReal>& steerResponseStates,
    PxVehicleArrayData<const PxVehicleWheelParams>& wheelParams,
    PxVehicleArrayData<const PxVehicleSuspensionParams>& suspensionParams,
    PxVehicleArrayData<const PxVehicleWheelActuationState>& actuationStates,
    PxVehicleArrayData<const PxVehicleSuspensionState>& suspensionStates,
    PxVehicleArrayData<const PxVehicleSuspensionComplianceState>& suspensionComplianceStates,
    PxVehicleArrayData<const PxVehicleTireSpeedState>& tireSpeedStates,
    PxVehicleArrayData<PxVehicleWheelRigidBody1dState>& wheelRigidBody1dStates,
    PxVehicleArrayData<PxVehicleWheelLocalPose>& wheelLocalPoses) {
    axleDescription = &axleDescription_;
    steerResponseStates.setData(steerResponseStates_);
    wheelParams.setData(wheelParams_);
    suspensionParams.setData(suspensionParams_);
    actuationStates.setData(actuationStates_);
    suspensionStates.setData(suspensionStates_);
    suspensionComplianceStates.setData(complianceStates_);
    tireSpeedStates.setData(tireSpeedStates_);
    wheelRigidBody1dStates.setData(wheel1dStates_);
    wheelLocalPoses.setData(wheelLocalPoses_);
}

void VehicleInstance::getDataForPhysXActorBeginComponent(
    const PxVehicleAxleDescription*& axleDescription, const PxVehicleCommandState*& commands,
    const PxVehicleEngineDriveTransmissionCommandState*& transmissionCommands,
    const PxVehicleGearboxParams*& gearParams, const PxVehicleGearboxState*& gearState,
    const PxVehicleEngineParams*& engineParams, PxVehiclePhysXActor*& physxActor,
    PxVehiclePhysXSteerState*& physxSteerState, PxVehiclePhysXConstraints*& physxConstraints,
    PxVehicleRigidBodyState*& rigidBodyState,
    PxVehicleArrayData<PxVehicleWheelRigidBody1dState>& wheelRigidBody1dStates,
    PxVehicleEngineState*& engineState) {
    axleDescription = &axleDescription_;
    commands = &commandState_;
    // Direct drive tidak punya mesin maupun girboks; keempatnya null, dan
    // komponen PhysX memang memeriksa null alih-alih menuntutnya ada.
    transmissionCommands = nullptr;
    gearParams = nullptr;
    gearState = nullptr;
    engineParams = nullptr;
    physxActor = &physxActor_;
    physxSteerState = &physxSteerState_;
    physxConstraints = &physxConstraints_;
    rigidBodyState = &rigidBodyState_;
    wheelRigidBody1dStates.setData(wheel1dStates_);
    engineState = nullptr;
}

void VehicleInstance::getDataForPhysXActorEndComponent(
    const PxVehicleAxleDescription*& axleDescription,
    const PxVehicleRigidBodyState*& rigidBodyState,
    PxVehicleArrayData<const PxVehicleWheelParams>& wheelParams,
    PxVehicleArrayData<const PxTransform>& wheelShapeLocalPoses,
    PxVehicleArrayData<const PxVehicleWheelRigidBody1dState>& wheelRigidBody1dStates,
    PxVehicleArrayData<const PxVehicleWheelLocalPose>& wheelLocalPoses,
    const PxVehicleGearboxState*& gearState, const PxReal*& throttle,
    PxVehiclePhysXActor*& physxActor) {
    axleDescription = &axleDescription_;
    rigidBodyState = &rigidBodyState_;
    wheelParams.setData(wheelParams_);
    wheelShapeLocalPoses.setData(wheelShapeLocalPoses_);
    wheelRigidBody1dStates.setData(wheel1dStates_);
    wheelLocalPoses.setData(wheelLocalPoses_);
    gearState = nullptr;
    throttle = &commandState_.throttle;
    physxActor = &physxActor_;
}

void VehicleInstance::getDataForPhysXConstraintComponent(
    const PxVehicleAxleDescription*& axleDescription,
    const PxVehicleRigidBodyState*& rigidBodyState,
    PxVehicleArrayData<const PxVehicleSuspensionParams>& suspensionParams,
    PxVehicleArrayData<const PxVehiclePhysXSuspensionLimitConstraintParams>& suspensionLimitParams,
    PxVehicleArrayData<const PxVehicleSuspensionState>& suspensionStates,
    PxVehicleArrayData<const PxVehicleSuspensionComplianceState>& suspensionComplianceStates,
    PxVehicleArrayData<const PxVehicleRoadGeometryState>& wheelRoadGeomStates,
    PxVehicleArrayData<const PxVehicleTireDirectionState>& tireDirectionStates,
    PxVehicleArrayData<const PxVehicleTireStickyState>& tireStickyStates,
    PxVehiclePhysXConstraints*& constraints) {
    axleDescription = &axleDescription_;
    rigidBodyState = &rigidBodyState_;
    suspensionParams.setData(suspensionParams_);
    suspensionLimitParams.setData(suspensionLimitParams_);
    suspensionStates.setData(suspensionStates_);
    suspensionComplianceStates.setData(complianceStates_);
    wheelRoadGeomStates.setData(roadGeomStates_);
    tireDirectionStates.setData(tireDirectionStates_);
    tireStickyStates.setData(tireStickyStates_);
    constraints = &physxConstraints_;
}

void VehicleInstance::getDataForPhysXRoadGeometrySceneQueryComponent(
    const PxVehicleAxleDescription*& axleDescription,
    const PxVehiclePhysXRoadGeometryQueryParams*& roadGeomParams,
    PxVehicleArrayData<const PxReal>& steerResponseStates,
    const PxVehicleRigidBodyState*& rigidBodyState,
    PxVehicleArrayData<const PxVehicleWheelParams>& wheelParams,
    PxVehicleArrayData<const PxVehicleSuspensionParams>& suspensionParams,
    PxVehicleArrayData<const PxVehiclePhysXMaterialFrictionParams>& materialFrictionParams,
    PxVehicleArrayData<PxVehicleRoadGeometryState>& roadGeometryStates,
    PxVehicleArrayData<PxVehiclePhysXRoadGeometryQueryState>& physxRoadGeometryStates) {
    axleDescription = &axleDescription_;
    roadGeomParams = &roadGeometryParams_;
    steerResponseStates.setData(steerResponseStates_);
    rigidBodyState = &rigidBodyState_;
    wheelParams.setData(wheelParams_);
    suspensionParams.setData(suspensionParams_);
    materialFrictionParams.setData(materialFrictionParams_);
    roadGeometryStates.setData(roadGeomStates_);
    physxRoadGeometryStates.setEmpty();
}

void VehicleInstance::getDataForDirectDriveCommandResponseComponent(
    const PxVehicleAxleDescription*& axleDescription,
    PxVehicleSizedArrayData<const PxVehicleBrakeCommandResponseParams>& brakeResponseParams,
    const PxVehicleDirectDriveThrottleCommandResponseParams*& throttleResponseParams,
    const PxVehicleSteerCommandResponseParams*& steerResponseParams,
    PxVehicleSizedArrayData<const PxVehicleAckermannParams>& ackermannParams,
    const PxVehicleCommandState*& commands,
    const PxVehicleDirectDriveTransmissionCommandState*& transmissionCommands,
    const PxVehicleRigidBodyState*& rigidBodyState,
    PxVehicleArrayData<PxReal>& brakeResponseStates,
    PxVehicleArrayData<PxReal>& throttleResponseStates,
    PxVehicleArrayData<PxReal>& steerResponseStates) {
    axleDescription = &axleDescription_;
    brakeResponseParams.setDataAndCount(brakeParams_, 2);
    throttleResponseParams = &throttleParams_;
    steerResponseParams = &steerParams_;
    ackermannParams.setEmpty();
    commands = &commandState_;
    transmissionCommands = &transmissionState_;
    rigidBodyState = &rigidBodyState_;
    brakeResponseStates.setData(brakeResponseStates_);
    throttleResponseStates.setData(throttleResponseStates_);
    steerResponseStates.setData(steerResponseStates_);
}

void VehicleInstance::getDataForDirectDriveActuationStateComponent(
    const PxVehicleAxleDescription*& axleDescription,
    PxVehicleArrayData<const PxReal>& brakeResponseStates,
    PxVehicleArrayData<const PxReal>& throttleResponseStates,
    PxVehicleArrayData<PxVehicleWheelActuationState>& actuationStates) {
    axleDescription = &axleDescription_;
    brakeResponseStates.setData(brakeResponseStates_);
    throttleResponseStates.setData(throttleResponseStates_);
    actuationStates.setData(actuationStates_);
}

void VehicleInstance::getDataForDirectDrivetrainComponent(
    const PxVehicleAxleDescription*& axleDescription,
    PxVehicleArrayData<const PxReal>& brakeResponseStates,
    PxVehicleArrayData<const PxReal>& throttleResponseStates,
    PxVehicleArrayData<const PxVehicleWheelParams>& wheelParams,
    PxVehicleArrayData<const PxVehicleWheelActuationState>& actuationStates,
    PxVehicleArrayData<const PxVehicleTireForce>& tireForces,
    PxVehicleArrayData<PxVehicleWheelRigidBody1dState>& wheelRigidBody1dStates) {
    axleDescription = &axleDescription_;
    brakeResponseStates.setData(brakeResponseStates_);
    throttleResponseStates.setData(throttleResponseStates_);
    wheelParams.setData(wheelParams_);
    actuationStates.setData(actuationStates_);
    tireForces.setData(tireForces_);
    wheelRigidBody1dStates.setData(wheel1dStates_);
}

}  // namespace sim::physics

#endif  // SIM_WITH_PHYSX
