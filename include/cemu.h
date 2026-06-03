#pragma once

union FPR_t {
    double fpr;
    struct
    {
        double fp0;
        double fp1;
    };
    struct
    {
        uint64_t guint;
    };
    struct
    {
        uint64_t fp0int;
        uint64_t fp1int;
    };
};

struct PPCInterpreter_t {
    uint32_t instructionPointer;
    uint32_t gpr[32];
    FPR_t fpr[32];
    uint32_t fpscr;
    uint8_t crNew[32]; // 0 -> bit not set, 1 -> bit set (upper 7 bits of each byte must always be zero) (cr0 starts at index 0, cr1 at index 4 ..)
    uint8_t xer_ca;    // carry from xer
    uint8_t LSQE;
    uint8_t PSE;
    // thread remaining cycles
    int32_t remainingCycles; // if this value goes below zero, the next thread is scheduled
    int32_t skippedCycles;   // if remainingCycles is set to zero to immediately end thread execution, this value holds the number of skipped cycles
    struct
    {
        uint32_t LR;
        uint32_t CTR;
        uint32_t XER;
        uint32_t UPIR;
        uint32_t UGQR[8];
    } sprNew;
};

enum VPADButtons : uint32_t {
    VPAD_BUTTON_NONE = 0,
    VPAD_BUTTON_A = 0x8000,
    VPAD_BUTTON_B = 0x4000,
    VPAD_BUTTON_X = 0x2000,
    VPAD_BUTTON_Y = 0x1000,
    VPAD_BUTTON_LEFT = 0x0800,
    VPAD_BUTTON_RIGHT = 0x0400,
    VPAD_BUTTON_UP = 0x0200,
    VPAD_BUTTON_DOWN = 0x0100,
    VPAD_BUTTON_ZL = 0x0080,
    VPAD_BUTTON_ZR = 0x0040,
    VPAD_BUTTON_L = 0x0020,
    VPAD_BUTTON_R = 0x0010,
    VPAD_BUTTON_PLUS = 0x0008,
    VPAD_BUTTON_MINUS = 0x0004,
    VPAD_BUTTON_HOME = 0x0002,
    VPAD_BUTTON_SYNC = 0x0001,
    VPAD_BUTTON_STICK_R = 0x00020000,
    VPAD_BUTTON_STICK_L = 0x00040000,
    VPAD_BUTTON_TV = 0x00010000,
    VPAD_STICK_R_EMULATION_LEFT = 0x04000000,
    VPAD_STICK_R_EMULATION_RIGHT = 0x02000000,
    VPAD_STICK_R_EMULATION_UP = 0x01000000,
    VPAD_STICK_R_EMULATION_DOWN = 0x00800000,
    VPAD_STICK_L_EMULATION_LEFT = 0x40000000,
    VPAD_STICK_L_EMULATION_RIGHT = 0x20000000,
    VPAD_STICK_L_EMULATION_UP = 0x10000000,
    VPAD_STICK_L_EMULATION_DOWN = 0x08000000,
};
ENABLE_BITMASK_OPERATORS(VPADButtons);

struct BEDir {
    BEVec3 x;
    BEVec3 y;
    BEVec3 z;

    BEDir() = default;
};

struct BETouchData {
    BEType<uint16_t> x;
    BEType<uint16_t> y;
    BEType<uint16_t> touch;
    BEType<uint16_t> validity;
};

struct VPADStatus {
    BEType<uint32_t> hold;
    BEType<uint32_t> trig;
    BEType<uint32_t> release;
    BEVec2 leftStick;
    BEVec2 rightStick;
    BEVec3 acc;
    BEType<float> accMagnitude;
    BEType<float> accAcceleration;
    BEVec2 accXY;
    BEVec3 gyroChange;
    BEVec3 gyroOrientation;
    int8_t vpadErr;
    uint8_t padding1[1];
    BETouchData tpData;
    BETouchData tpProcessed1;
    BETouchData tpProcessed2;
    uint8_t padding2[2];
    BEDir dir;
    uint8_t headphoneStatus;
    uint8_t padding3[3];
    BEVec3 magnet;
    uint8_t slideVolume;
    uint8_t batteryLevel;
    uint8_t micStatus;
    uint8_t slideVolume2;
    uint8_t padding4[8];
};
BVR_SIZE_CHECK(sizeof(VPADStatus) == 0xAC, "VPADStatus size mismatch");

typedef void (*osLib_registerHLEFunctionPtr_t)(const char* libraryName, const char* functionName, void (*osFunction)(PPCInterpreter_t* hCPU));
typedef void* (*memory_getBasePtr_t)();
typedef uint64_t (*gameMeta_getTitleIdPtr_t)();