#pragma once

// Standard C++ includes
#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <numeric>
#include <string>
#include <unordered_set>
#include <regex>

// OpenCL includes
#define CL_USE_DEPRECATED_OPENCL_1_2_APIS
#include <CL/cl.hpp>

// GeNN includes
#include "backendExport.h"

// GeNN code generator includes
#include "code_generator/backendBase.h"
#include "code_generator/codeStream.h"
#include "code_generator/substitutions.h"

// OpenCL backend includes
#include "presynapticUpdateStrategy.h"

// Forward declarations
namespace filesystem
{
    class path;
}

//--------------------------------------------------------------------------
// CodeGenerator::OpenCL::DeviceSelectMethod
//--------------------------------------------------------------------------
namespace CodeGenerator
{
namespace OpenCL
{
//! Methods for selecting OpenCL device
enum class DeviceSelect
{
    OPTIMAL,        //!< Pick optimal device based on how well kernels can be simultaneously simulated and occupancy
    MOST_MEMORY,    //!< Pick device with most global memory
    MANUAL,         //!< Use device specified by user
};

//--------------------------------------------------------------------------
// CodeGenerator::OpenCL::WorkGroupSizeSelect
//--------------------------------------------------------------------------
//! Methods for selecting OpenCL kernel workgroup size
enum class WorkGroupSizeSelect
{
    OCCUPANCY,  //!< Pick optimal workgroup size for each kernel based on occupancy
    MANUAL,     //!< Use workgroup sizes specified by user
};

//--------------------------------------------------------------------------
// Kernel
//--------------------------------------------------------------------------
//! Kernels generated by OpenCL backend
enum Kernel
{
    KernelNeuronUpdate,
    KernelPresynapticUpdate,
    KernelPostsynapticUpdate,
    KernelSynapseDynamicsUpdate,
    KernelInitialize,
    KernelInitializeSparse,
    KernelPreNeuronReset,
    KernelPreSynapseReset,
    KernelMax
};
//! Programs generated by OpenCL backend
enum Program
{
    ProgramInitialize,
    ProgramNeuronsUpdate,
    ProgramSynapsesUpdate,
    ProgramMax
};

//--------------------------------------------------------------------------
// Type definitions
//--------------------------------------------------------------------------
//! Array of workgroup sizes for each kernel
using KernelWorkGroupSize = std::array<size_t, KernelMax>;

//--------------------------------------------------------------------------
// CodeGenerator::OpenCL::Preferences
//--------------------------------------------------------------------------
//! Preferences for OpenCL backend
struct Preferences : public PreferencesBase
{
    Preferences()
    {
        std::fill(manualWorkGroupSizes.begin(), manualWorkGroupSizes.end(), 32);
    }

    //! Should we use the constant cache for storing merged structures - improves performance but may overflow for large models
    bool useConstantCacheForMergedStructs = true;

    //! How to select GPU device
    DeviceSelect deviceSelectMethod = DeviceSelect::OPTIMAL;

    //! If device select method is set to DeviceSelect::MANUAL, id of device to use
    unsigned int manualDeviceID = 0;

    //! How to select OpenCL workgroup size
    WorkGroupSizeSelect workGroupSizeSelectMethod = WorkGroupSizeSelect::OCCUPANCY;

    //! If block size select method is set to BlockSizeSelect::MANUAL, block size to use for each kernel
    KernelWorkGroupSize manualWorkGroupSizes;

    //! TO BE IMPLEMENTED - Additional libraries in kernels for passing to program.build
};

//--------------------------------------------------------------------------
// CodeGenerator::OpenCL::Backend
//--------------------------------------------------------------------------
class BACKEND_EXPORT Backend : public BackendBase
{
public:
    Backend(const KernelWorkGroupSize& kernelWorkGroupSizes, const Preferences& preferences,
        int localHostID, const std::string& scalarType, int device);

    //--------------------------------------------------------------------------
    // CodeGenerator::BackendBase:: virtuals
    //--------------------------------------------------------------------------
    virtual void genNeuronUpdate(CodeStream& os, const ModelSpecInternal& model, NeuronGroupSimHandler simHandler, NeuronGroupHandler wuVarUpdateHandler) const override;

    virtual void genSynapseUpdate(CodeStream& os, const ModelSpecInternal& model,
        SynapseGroupHandler wumThreshHandler, SynapseGroupHandler wumSimHandler, SynapseGroupHandler wumEventHandler,
        SynapseGroupHandler postLearnHandler, SynapseGroupHandler synapseDynamicsHandler) const override;

    virtual void genInit(CodeStream& os, const ModelSpecInternal& model,
        NeuronGroupHandler localNGHandler, NeuronGroupHandler remoteNGHandler,
        SynapseGroupHandler sgDenseInitHandler, SynapseGroupHandler sgSparseConnectHandler,
        SynapseGroupHandler sgSparseInitHandler) const override;

    virtual void genDefinitionsPreamble(CodeStream& os) const override;
    virtual void genDefinitionsInternalPreamble(CodeStream& os) const override;
    virtual void genRunnerPreamble(CodeStream& os) const override;
    virtual void genAllocateMemPreamble(CodeStream& os, const ModelSpecInternal& model) const override;
    virtual void genAllocateMemPostamble(CodeStream& os, const ModelSpecInternal& model) const override;
    virtual void genStepTimeFinalisePreamble(CodeStream& os, const ModelSpecInternal& model) const override;

    virtual void genVariableDefinition(CodeStream& definitions, CodeStream& definitionsInternal, const std::string& type, const std::string& name, VarLocation loc) const override;
    virtual void genVariableImplementation(CodeStream& os, const std::string& type, const std::string& name, VarLocation loc) const override;
    virtual MemAlloc genVariableAllocation(CodeStream& os, const std::string& type, const std::string& name, VarLocation loc, size_t count) const override;
    virtual void genVariableFree(CodeStream& os, const std::string& name, VarLocation loc) const override;

    virtual void genExtraGlobalParamDefinition(CodeStream& definitions, const std::string& type, const std::string& name, VarLocation loc) const override;
    virtual void genExtraGlobalParamImplementation(CodeStream& os, const std::string& type, const std::string& name, VarLocation loc) const override;
    virtual void genExtraGlobalParamAllocation(CodeStream& os, const std::string& type, const std::string& name, VarLocation loc) const override;
    virtual void genExtraGlobalParamPush(CodeStream& os, const std::string& type, const std::string& name, VarLocation loc) const override;
    virtual void genExtraGlobalParamPull(CodeStream& os, const std::string& type, const std::string& name, VarLocation loc) const override;

    virtual void genPopVariableInit(CodeStream& os, VarLocation loc, const Substitutions& kernelSubs, Handler handler) const override;
    virtual void genVariableInit(CodeStream& os, VarLocation loc, size_t count, const std::string& indexVarName,
        const Substitutions& kernelSubs, Handler handler) const override;
    virtual void genSynapseVariableRowInit(CodeStream& os, VarLocation loc, const SynapseGroupInternal& sg,
        const Substitutions& kernelSubs, Handler handler) const override;

    virtual void genVariablePush(CodeStream& os, const std::string& type, const std::string& name, VarLocation loc, bool autoInitialized, size_t count) const override;
    virtual void genVariablePull(CodeStream& os, const std::string& type, const std::string& name, VarLocation loc, size_t count) const override;

    virtual void genCurrentVariablePush(CodeStream& os, const NeuronGroupInternal& ng, const std::string& type, const std::string& name, VarLocation loc) const override;
    virtual void genCurrentVariablePull(CodeStream& os, const NeuronGroupInternal& ng, const std::string& type, const std::string& name, VarLocation loc) const override;

    virtual void genCurrentTrueSpikePush(CodeStream& os, const NeuronGroupInternal& ng) const override
    {
        genCurrentSpikePush(os, ng, false);
    }
    virtual void genCurrentTrueSpikePull(CodeStream& os, const NeuronGroupInternal& ng) const override
    {
        genCurrentSpikePull(os, ng, false);
    }
    virtual void genCurrentSpikeLikeEventPush(CodeStream& os, const NeuronGroupInternal& ng) const override
    {
        genCurrentSpikePush(os, ng, true);
    }
    virtual void genCurrentSpikeLikeEventPull(CodeStream& os, const NeuronGroupInternal& ng) const override
    {
        genCurrentSpikePull(os, ng, true);
    }

    virtual MemAlloc genGlobalRNG(CodeStream& definitions, CodeStream& definitionsInternal, CodeStream& runner, CodeStream& allocations, CodeStream& free, const ModelSpecInternal& model) const override;
    virtual MemAlloc genPopulationRNG(CodeStream& definitions, CodeStream& definitionsInternal, CodeStream& runner,
        CodeStream& allocations, CodeStream& free, const std::string& name, size_t count) const override;
    virtual void genTimer(CodeStream& definitions, CodeStream& definitionsInternal, CodeStream& runner,
        CodeStream& allocations, CodeStream& free, CodeStream& stepTimeFinalise,
        const std::string& name, bool updateInStepTime) const override;

    virtual void genMakefilePreamble(std::ostream& os) const override;
    virtual void genMakefileLinkRule(std::ostream& os) const override;
    virtual void genMakefileCompileRule(std::ostream& os) const override;

    virtual void genMSBuildConfigProperties(std::ostream& os) const override;
    virtual void genMSBuildImportProps(std::ostream& os) const override;
    virtual void genMSBuildItemDefinitions(std::ostream& os) const override;
    virtual void genMSBuildCompileModule(const std::string& moduleName, std::ostream& os) const override;
    virtual void genMSBuildImportTarget(std::ostream& os) const override;

    virtual std::string getVarPrefix() const override { return "d_"; }

    virtual bool isGlobalRNGRequired(const ModelSpecInternal& model) const override;
    virtual bool isSynRemapRequired() const override { return true; }
    virtual bool isPostsynapticRemapRequired() const override { return true; }

    //! How many bytes of memory does 'device' have
    virtual size_t getDeviceMemoryBytes() const override { return m_ChosenDevice.getInfo<CL_DEVICE_GLOBAL_MEM_SIZE>(); }

    //--------------------------------------------------------------------------
    // Public API
    //--------------------------------------------------------------------------
    const cl::Device& getChosenOpenCLDevice() const { return m_ChosenDevice; }
    int getChosenDeviceID() const { return m_ChosenDeviceID; }
    int getRuntimeVersion() const { return m_RuntimeVersion; }

    std::string getFloatAtomicAdd(const std::string& ftype, const char* memoryType = "global") const;

    size_t getKernelBlockSize(Kernel kernel) const { return m_KernelWorkGroupSizes.at(kernel); }

    //--------------------------------------------------------------------------
    // Static API
    //--------------------------------------------------------------------------
    static size_t getNumPresynapticUpdateThreads(const SynapseGroupInternal& sg);
    static size_t getNumPostsynapticUpdateThreads(const SynapseGroupInternal& sg);
    static size_t getNumSynapseDynamicsThreads(const SynapseGroupInternal& sg);

    //! Register a new presynaptic update strategy
    /*! This function should be called with strategies in ascending order of preference */
    static void addPresynapticUpdateStrategy(PresynapticUpdateStrategy::Base* strategy);

    //--------------------------------------------------------------------------
    // Constants
    //--------------------------------------------------------------------------
    static const char* KernelNames[KernelMax];
    static const char* ProgramNames[ProgramMax];

private:
    //--------------------------------------------------------------------------
    // Type definitions
    //--------------------------------------------------------------------------
    template<typename T>
    using GetPaddedGroupSizeFunc = std::function<size_t(const T&)>;

    template<typename T>
    using FilterGroupFunc = std::function<bool(const T&)>;

    //--------------------------------------------------------------------------
    // Private methods
    //--------------------------------------------------------------------------
    template<typename T>
    void genParallelGroup(CodeStream& os, const Substitutions& kernelSubs, const std::map<std::string, T>& groups, size_t& idStart, std::map<std::string, std::string>& params,
        GetPaddedGroupSizeFunc<T> getPaddedSizeFunc,
        FilterGroupFunc<T> filter,
        GroupHandler<T> handler) const
    {
        // Populate neuron update groups
        for (const auto& g : groups) {
            // If this synapse group should be processed
            Substitutions popSubs(&kernelSubs);
            if (filter(g.second)) {
                const size_t paddedSize = getPaddedSizeFunc(g.second);

                os << "// " << g.first << std::endl;

                // If this is the first  group
                if (idStart == 0) {
                    os << "if(id < " << paddedSize << ")" << CodeStream::OB(1);
                    popSubs.addVarSubstitution("id", "id");
                }
                else {
                    os << "if(id >= " << idStart << " && id < " << idStart + paddedSize << ")" << CodeStream::OB(1);
                    os << "const unsigned int lid = id - " << idStart << ";" << std::endl;
                    popSubs.addVarSubstitution("id", "lid");
                }

                std::stringstream subOsStream;
                CodeStream subOs(subOsStream);

                handler(subOs, g.second, popSubs);

                std::string code = subOsStream.str();
                os << code;

                // Collect device variables in code
                std::regex rgx("\\b" + getVarPrefix() + "\\w+\\b");
                for (std::sregex_iterator it(code.begin(), code.end(), rgx), end; it != end; it++) {
                    params.insert({ it->str(), "__global scalar*" });
                }

                idStart += paddedSize;
                os << CodeStream::CB(1) << std::endl;
            }
        }
    }

    template<typename T>
    void genParallelGroup(CodeStream& os, const Substitutions& kernelSubs, const std::map<std::string, T>& groups, size_t& idStart, std::map<std::string, std::string>& params,
        GetPaddedGroupSizeFunc<T> getPaddedSizeFunc,
        GroupHandler<T> handler) const
    {
        genParallelGroup<T>(os, kernelSubs, groups, idStart, params, getPaddedSizeFunc,
            [](const T&) { return true; }, handler);
    }

    void genEmitSpike(CodeStream& os, const Substitutions& subs, const std::string& suffix) const;

    void genCurrentSpikePush(CodeStream& os, const NeuronGroupInternal& ng, bool spikeEvent) const;
    void genCurrentSpikePull(CodeStream& os, const NeuronGroupInternal& ng, bool spikeEvent) const;

    void genKernelDimensions(CodeStream& os, Kernel kernel, size_t numThreads) const;

    void genKernelHostArgs(CodeStream& os, Kernel kernel, const std::map<std::string, std::string>& params) const;

    //! Adds a type - both to backend base's list of sized types but also to device types set
    void addDeviceType(const std::string& type, size_t size);

    //! Is type a a device only type?
    bool isDeviceType(const std::string& type) const;

    void divideKernelStreamInParts(CodeStream& os, std::stringstream& kernelCode, int partLength) const;

    //--------------------------------------------------------------------------
    // Private static methods
    //--------------------------------------------------------------------------
    // Get appropriate presynaptic update strategy to use for this synapse group
    static const PresynapticUpdateStrategy::Base* getPresynapticUpdateStrategy(const SynapseGroupInternal& sg);

    //--------------------------------------------------------------------------
    // Members
    //--------------------------------------------------------------------------
    const KernelWorkGroupSize m_KernelWorkGroupSizes;
    const Preferences m_Preferences;

    const int m_ChosenDeviceID;
    cl::Device m_ChosenDevice;

    int m_RuntimeVersion;

    //! Types that are only supported on device i.e. should never be exposed to user code
    std::unordered_set<std::string> m_DeviceTypes;

    //--------------------------------------------------------------------------
    // Static members
    //--------------------------------------------------------------------------
    static std::vector<PresynapticUpdateStrategy::Base*> s_PresynapticUpdateStrategies;
};
}   // OpenCL
}   // CodeGenerator
