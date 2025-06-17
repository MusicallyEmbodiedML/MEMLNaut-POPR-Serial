// #include "src/memllib/interface/InterfaceBase.hpp"
#include "display.hpp"
#include "src/memllib/audio/AudioAppBase.hpp"
#include "src/memllib/audio/AudioDriver.hpp"
#include "src/memllib/hardware/memlnaut/MEMLNaut.hpp"
#include <memory>
#include "IMLInterface.hpp"
#include "interfaceRL.hpp"
#include "hardware/structs/bus_ctrl.h"
#include "PAFSynthAudioApp.hpp"


#define APP_SRAM __not_in_flash("app")

display APP_SRAM scr;

bool core1_disable_systick = true;
bool core1_separate_stack = true;

uint32_t get_rosc_entropy_seed(int bits) {
    uint32_t seed = 0;
    for (int i = 0; i < bits; ++i) {
        // Wait for a bit of time to allow jitter to accumulate
        busy_wait_us_32(5);
        // Pull LSB from ROSC rand output
        seed <<= 1;
        seed |= (rosc_hw->randombit & 1);
    }
    return seed;
}


// Global objects
std::shared_ptr<IMLInterface> APP_SRAM interfaceIML;
std::shared_ptr<interfaceRL> APP_SRAM RLInterface;

std::shared_ptr<PAFSynthAudioApp> __scratch_y("audio") audio_app;

// Inter-core communication
volatile bool APP_SRAM core_0_ready = false;
volatile bool APP_SRAM core_1_ready = false;
volatile bool APP_SRAM serial_ready = false;
volatile bool APP_SRAM interface_ready = false;


// We're only bound to the joystick inputs (x, y, rotate)
constexpr size_t kN_InputParams = 3;

// Add these macros near other globals
#define MEMORY_BARRIER() __sync_synchronize()
#define WRITE_VOLATILE(var, val) do { MEMORY_BARRIER(); (var) = (val); MEMORY_BARRIER(); } while (0)
#define READ_VOLATILE(var) ({ MEMORY_BARRIER(); typeof(var) __temp = (var); MEMORY_BARRIER(); __temp; })


void bind_RL_interface(std::shared_ptr<interfaceRL> interface)
{
    // Set up momentary switch callbacks
    MEMLNaut::Instance()->setMomA1Callback([interface] () {
        static APP_SRAM std::vector<String> msgs = {"Wow, incredible", "Awesome", "That's amazing", "Unbelievable+","I love it!!","More of this","Yes!!!!","A-M-A-Z-I-N-G"};
        String msg = msgs[rand() % msgs.size()];
        interface->storeExperience(1.f);
        Serial.println(msg);

        scr.post(msg);
    });
    MEMLNaut::Instance()->setMomA2Callback([interface] () {
        static APP_SRAM std::vector<String> msgs = {"Awful!","wtf? that sucks","Get rid of this sound","Totally shite","I hate this","Why even bother?","New sound please!","No, please no!!!","Thumbs down"};
        String msg = msgs[rand() % msgs.size()];
        interface->storeExperience(-1.f);
        Serial.println(msg);
        scr.post(msg);
    });
    MEMLNaut::Instance()->setMomB1Callback([interface] () {
        interface->randomiseTheActor();
        interface->generateAction(true);
        Serial.println("The Actor is confused");
        scr.post("Actor: i'm confused");
    });
    MEMLNaut::Instance()->setMomB2Callback([interface] () {
        interface->randomiseTheCritic();
        interface->generateAction(true);
        Serial.println("The Critic is confounded");
        scr.post("Critic: totally confounded");
    });
    // Set up ADC callbacks
    MEMLNaut::Instance()->setJoyXCallback([interface] (float value) {
        interface->setState(0, value);
    });
    MEMLNaut::Instance()->setJoyYCallback([interface] (float value) {
        interface->setState(1, value);
    });
    MEMLNaut::Instance()->setJoyZCallback([interface] (float value) {
        interface->setState(2, value);
    });

    MEMLNaut::Instance()->setRVGain1Callback([interface] (float value) {
        AudioDriver::setDACVolume(value);
    });

    MEMLNaut::Instance()->setRVX1Callback([interface] (float value) {
        size_t divisor = 1 + (value * 100);
        String msg = "Optimise every " + String(divisor);
        scr.post(msg);
        interface->setOptimiseDivisor(divisor);
        Serial.println(msg);
    });


    // Set up loop callback
    MEMLNaut::Instance()->setLoopCallback([interface] () {
        interface->optimiseSometimes();
        interface->generateAction();
    });


}

void bind_IML_interface(std::shared_ptr<IMLInterface> interface)
{
    // Set up momentary switch callbacks
    MEMLNaut::Instance()->setMomA1Callback([interface] () {
        interface->Randomise();
    });
    MEMLNaut::Instance()->setMomA2Callback([interface] () {
        interface->ClearData();
    });

    // Set up toggle switch callbacks
    MEMLNaut::Instance()->setTogA1Callback([interface] (bool state) {
        interface->SetTrainingMode(state ? IMLInterface::TRAINING_MODE : IMLInterface::INFERENCE_MODE);
    });
    MEMLNaut::Instance()->setJoySWCallback([interface] (bool state) {
        interface->SaveInput(state ? IMLInterface::STORE_VALUE_MODE : IMLInterface::STORE_POSITION_MODE);
    });

    // Set up ADC callbacks
    MEMLNaut::Instance()->setJoyXCallback([interface] (float value) {
        interface->SetInput(0, value);
    });
    MEMLNaut::Instance()->setJoyYCallback([interface] (float value) {
        interface->SetInput(1, value);
    });
    MEMLNaut::Instance()->setJoyZCallback([interface] (float value) {
        interface->SetInput(2, value);
    });
    MEMLNaut::Instance()->setRVZ1Callback([interface] (float value) {
        // Scale value from 0-1 range to 1-3000
        value = 1.0f + (value * 2999.0f);
        interface->SetIterations(static_cast<size_t>(value));
    });

    // Set up loop callback
    MEMLNaut::Instance()->setLoopCallback([interface] () {
        interface->ProcessInput();
    });

    MEMLNaut::Instance()->setRVGain1Callback([interface] (float value) {
        AudioDriver::setDACVolume(value);
    });
}

enum MLMODES {IML, RL};
MLMODES APP_SRAM mlMode = RL;


struct repeating_timer APP_SRAM timerDisplay;
inline bool __not_in_flash_func(displayUpdate)(__unused struct repeating_timer *t) {
    scr.update();
    return true;
}

void setup()
{

    scr.setup();
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS |
        BUSCTRL_BUS_PRIORITY_DMA_R_BITS | BUSCTRL_BUS_PRIORITY_PROC1_BITS;

    uint32_t seed = get_rosc_entropy_seed(32);
    srand(seed);

    Serial.begin(115200);
    // while (!Serial) {}
    Serial.println("Serial initialised.");
    WRITE_VOLATILE(serial_ready, true);

    // Setup board
    MEMLNaut::Initialize();
    pinMode(33, OUTPUT);

    switch(mlMode) {
        case IML: {
            {
                auto temp_interface = std::make_shared<IMLInterface>();
                temp_interface->setup(kN_InputParams, PAFSynthAudioApp::kN_Params);
                MEMORY_BARRIER();
                interfaceIML = temp_interface;
                MEMORY_BARRIER();
            }
            // Setup interface with memory barrier protection
            WRITE_VOLATILE(interface_ready, true);
            // Bind interface after ensuring it's fully initialized
            bind_IML_interface(interfaceIML);
            Serial.println("Bound IML interface to MEMLNaut.");
        }
        break;
        case RL: {
            {
                auto temp_interface = std::make_shared<interfaceRL>();
                temp_interface->setup(kN_InputParams, PAFSynthAudioApp::kN_Params);
                MEMORY_BARRIER();
                RLInterface = temp_interface;
                MEMORY_BARRIER();
            }
            // Setup interface with memory barrier protection
            WRITE_VOLATILE(interface_ready, true);
            // Bind interface after ensuring it's fully initialized
            bind_RL_interface(RLInterface);
            Serial.println("Bound RL interface to MEMLNaut.");
        }
        break;
    }


    WRITE_VOLATILE(core_0_ready, true);
    while (!READ_VOLATILE(core_1_ready)) {
        MEMORY_BARRIER();
        delay(1);
    }

    scr.post("MEMLNaut: let's go!");
    add_repeating_timer_ms(-39, displayUpdate, NULL, &timerDisplay);

    Serial.println("Finished initialising core 0.");
}

void loop()
{


    MEMLNaut::Instance()->loop();
    static int AUDIO_MEM blip_counter = 0;
    if (blip_counter++ > 100) {
        blip_counter = 0;
        Serial.println(".");
        // Blink LED
        digitalWrite(33, HIGH);
    } else {
        // Un-blink LED
        digitalWrite(33, LOW);
    }
    delay(10); // Add a small delay to avoid flooding the serial output
}

void setup1()
{
    while (!READ_VOLATILE(serial_ready)) {
        MEMORY_BARRIER();
        delay(1);
    }

    while (!READ_VOLATILE(interface_ready)) {
        MEMORY_BARRIER();
        delay(1);
    }


    // Create audio app with memory barrier protection
    {
        auto temp_audio_app = std::make_shared<PAFSynthAudioApp>();
        std::shared_ptr<InterfaceBase> selectedInterface;

        if (mlMode == IML) {
            selectedInterface = std::dynamic_pointer_cast<InterfaceBase>(interfaceIML);
        } else {
            selectedInterface = std::dynamic_pointer_cast<InterfaceBase>(RLInterface);
        }

        temp_audio_app->Setup(AudioDriver::GetSampleRate(), selectedInterface);
        // temp_audio_app->Setup(AudioDriver::GetSampleRate(), dynamic_cast<std::shared_ptr<InterfaceBase>> (mlMode == IML ? interfaceIML : RLInterface));
        MEMORY_BARRIER();
        audio_app = temp_audio_app;
        MEMORY_BARRIER();
    }

    // Start audio driver
    AudioDriver::Setup();

    WRITE_VOLATILE(core_1_ready, true);
    while (!READ_VOLATILE(core_0_ready)) {
        MEMORY_BARRIER();
        delay(1);
    }

    Serial.println("Finished initialising core 1.");
}

void loop1()
{
    // Audio app parameter processing loop
    audio_app->loop();
    delay(1);
}

