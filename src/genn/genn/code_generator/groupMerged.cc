#include "code_generator/groupMerged.h"

// PLOG includes
#include <plog/Log.h>

// GeNN includes
#include "modelSpecInternal.h"

// GeNN code generator includes
#include "code_generator/backendBase.h"
#include "code_generator/codeGenUtils.h"
#include "code_generator/codeStream.h"
#include "code_generator/mergedStructGenerator.h"

//----------------------------------------------------------------------------
// CodeGenerator::NeuronSpikeQueueUpdateMergedGroup
//----------------------------------------------------------------------------
CodeGenerator::NeuronSpikeQueueUpdateMergedGroup::NeuronSpikeQueueUpdateMergedGroup(size_t index, const std::vector<std::reference_wrapper<const NeuronGroupInternal>> &groups)
    : CodeGenerator::GroupMerged<NeuronGroupInternal>(index, groups)
{
}
//----------------------------------------------------------------------------
void CodeGenerator::NeuronSpikeQueueUpdateMergedGroup::generate(const BackendBase &backend, CodeStream &definitionsInternal,
                                                                CodeStream &definitionsInternalFunc, CodeStream &definitionsInternalVar,
                                                                CodeStream &runnerVarDecl, CodeStream &runnerMergedStructAlloc,
                                                                MergedStructData &mergedStructData, const std::string &precision) const
{
    MergedStructGenerator<NeuronSpikeQueueUpdateMergedGroup> gen(*this, precision);

    if(getArchetype().isDelayRequired()) {
        gen.addField("unsigned int", "numDelaySlots",
                     [](const NeuronGroupInternal &ng, size_t) { return std::to_string(ng.getNumDelaySlots()); });

        gen.addField("volatile unsigned int*", "spkQuePtr",
                     [&backend](const NeuronGroupInternal &ng, size_t)
                     {
                         return "getSymbolAddress(" + backend.getScalarPrefix() + "spkQuePtr" + ng.getName() + ")";
                     });
    }

    gen.addPointerField("unsigned int", "spkCnt", backend.getArrayPrefix() + "glbSpkCnt");

    if(getArchetype().isSpikeEventRequired()) {
        gen.addPointerField("unsigned int", "spkCntEvnt", backend.getArrayPrefix() + "glbSpkCntEvnt");
    }


    // Generate structure definitions and instantiation
    gen.generate(backend, definitionsInternal, definitionsInternalFunc, definitionsInternalVar, runnerVarDecl, runnerMergedStructAlloc,
                 mergedStructData, "NeuronSpikeQueueUpdate");
}
//----------------------------------------------------------------------------
void CodeGenerator::NeuronSpikeQueueUpdateMergedGroup::genMergedGroupSpikeCountReset(CodeStream &os) const
{
    if(getArchetype().isDelayRequired()) { // with delay
        if(getArchetype().isSpikeEventRequired()) {
            os << "group.spkCntEvnt[*group.spkQuePtr] = 0;" << std::endl;
        }
        if(getArchetype().isTrueSpikeRequired()) {
            os << "group.spkCnt[*group.spkQuePtr] = 0;" << std::endl;
        }
        else {
            os << "group.spkCnt[0] = 0;" << std::endl;
        }
    }
    else { // no delay
        if(getArchetype().isSpikeEventRequired()) {
            os << "group.spkCntEvnt[0] = 0;" << std::endl;
        }
        os << "group.spkCnt[0] = 0;" << std::endl;
    }
}

//----------------------------------------------------------------------------
// CodeGenerator::NeuronGroupMergedBase
//----------------------------------------------------------------------------
bool CodeGenerator::NeuronGroupMergedBase::isParamHeterogeneous(size_t index) const
{
    return CodeGenerator::GroupMerged<NeuronGroupInternal>::isParamValueHeterogeneous(
        index, [](const NeuronGroupInternal &ng) { return ng.getParams(); });
}
//----------------------------------------------------------------------------
bool CodeGenerator::NeuronGroupMergedBase::isDerivedParamHeterogeneous(size_t index) const
{
    return CodeGenerator::GroupMerged<NeuronGroupInternal>::isParamValueHeterogeneous(
        index, [](const NeuronGroupInternal &ng) { return ng.getDerivedParams(); });
}
//----------------------------------------------------------------------------
bool CodeGenerator::NeuronGroupMergedBase::isCurrentSourceParamHeterogeneous(size_t childIndex, size_t paramIndex) const
{
    // If parameter isn't referenced in code, there's no point implementing it hetereogeneously!
    const auto *csm = getArchetype().getCurrentSources().at(childIndex)->getCurrentSourceModel();
    const std::string paramName = csm->getParamNames().at(paramIndex);
    if(csm->getInjectionCode().find("$(" + paramName + ")") == std::string::npos) {
        return false;
    }
    // Otherwise, return whether values across all groups are heterogeneous
    else {
        return isChildParamValueHeterogeneous(childIndex, paramIndex, m_SortedCurrentSources,
                                              [](const CurrentSourceInternal *cs) { return cs->getParams();  });
    }
}
//----------------------------------------------------------------------------
bool CodeGenerator::NeuronGroupMergedBase::isCurrentSourceDerivedParamHeterogeneous(size_t childIndex, size_t paramIndex) const
{
    // If derived parameter isn't referenced in code, there's no point implementing it hetereogeneously!
    const auto *csm = getArchetype().getCurrentSources().at(childIndex)->getCurrentSourceModel();
    const std::string derivedParamName = csm->getDerivedParams().at(paramIndex).name;
    if(csm->getInjectionCode().find("$(" + derivedParamName + ")") == std::string::npos) {
        return false;
    }
    // Otherwise, return whether values across all groups are heterogeneous
    else {
        return isChildParamValueHeterogeneous(childIndex, paramIndex, m_SortedCurrentSources,
                                              [](const CurrentSourceInternal *cs) { return cs->getDerivedParams();  });
    }
}
//----------------------------------------------------------------------------
CodeGenerator::NeuronGroupMergedBase::NeuronGroupMergedBase(size_t index, bool init, const std::vector<std::reference_wrapper<const NeuronGroupInternal>> &groups)
    : CodeGenerator::GroupMerged<NeuronGroupInternal>(index, groups)
{
    // Build vector of vectors containing each child group's merged in syns, ordered to match those of the archetype group
    orderNeuronGroupChildren(m_SortedMergedInSyns, &NeuronGroupInternal::getMergedInSyn,
                             [init](const std::pair<SynapseGroupInternal *, std::vector<SynapseGroupInternal *>> &a,
                                    const std::pair<SynapseGroupInternal *, std::vector<SynapseGroupInternal *>> &b)
                             {
                                 return init ? a.first->canPSInitBeMerged(*b.first) : a.first->canPSBeMerged(*b.first);
                             });

    // Build vector of vectors containing each child group's current sources, ordered to match those of the archetype group
    orderNeuronGroupChildren(m_SortedCurrentSources, &NeuronGroupInternal::getCurrentSources,
                             [init](const CurrentSourceInternal *a, const CurrentSourceInternal *b)
                             {
                                 return init ? a->canInitBeMerged(*b) : a->canBeMerged(*b);
                             });

    // Build vector of vectors containing each child group's incoming synapse groups
    // with postsynaptic updates, ordered to match those of the archetype group
    const auto inSynWithPostCode = getArchetype().getInSynWithPostCode();
    orderNeuronGroupChildren(inSynWithPostCode, m_SortedInSynWithPostCode, &NeuronGroupInternal::getInSynWithPostCode,
                             [init](const SynapseGroupInternal *a, const SynapseGroupInternal *b)
                             {
                                 return init ? a->canWUPostInitBeMerged(*b) : a->canWUPostBeMerged(*b);
                             });

    // Build vector of vectors containing each child group's incoming synapse groups
    // with postsynaptic updates, ordered to match those of the archetype group
    const auto outSynWithPreCode = getArchetype().getOutSynWithPreCode();
    orderNeuronGroupChildren(outSynWithPreCode, m_SortedOutSynWithPreCode, &NeuronGroupInternal::getOutSynWithPreCode,
                             [init](const SynapseGroupInternal *a, const SynapseGroupInternal *b)
                             {
                                 return init ? a->canWUPreInitBeMerged(*b) : a->canWUPreBeMerged(*b);
                             });
}
//----------------------------------------------------------------------------
void CodeGenerator::NeuronGroupMergedBase::generate(const BackendBase &backend, CodeStream &definitionsInternal,
                                                    CodeStream &definitionsInternalFunc, CodeStream &definitionsInternalVar,
                                                    CodeStream &runnerVarDecl, CodeStream &runnerMergedStructAlloc,
                                                    MergedStructData &mergedStructData, const std::string &precision,
                                                    const std::string &timePrecision, bool init) const
{
    MergedStructGenerator<NeuronGroupMergedBase> gen(*this, precision);

    gen.addField("unsigned int", "numNeurons",
                 [](const NeuronGroupInternal &ng, size_t) { return std::to_string(ng.getNumNeurons()); });

    gen.addPointerField("unsigned int", "spkCnt", backend.getArrayPrefix() + "glbSpkCnt");
    gen.addPointerField("unsigned int", "spk", backend.getArrayPrefix() + "glbSpk");

    if(getArchetype().isSpikeEventRequired()) {
        gen.addPointerField("unsigned int", "spkCntEvnt", backend.getArrayPrefix() + "glbSpkCntEvnt");
        gen.addPointerField("unsigned int", "spkEvnt", backend.getArrayPrefix() + "glbSpkEvnt");
    }

    if(getArchetype().isDelayRequired()) {
        gen.addField("volatile unsigned int*", "spkQuePtr",
                     [&backend](const NeuronGroupInternal &ng, size_t)
                     {
                         return "getSymbolAddress(" + backend.getScalarPrefix() + "spkQuePtr" + ng.getName() + ")";
                     });
    }

    if(getArchetype().isSpikeTimeRequired()) {
        gen.addPointerField(timePrecision, "sT", backend.getArrayPrefix() + "sT");
    }

    if(backend.isPopulationRNGRequired() && getArchetype().isSimRNGRequired()) {
        gen.addPointerField("curandState", "rng", backend.getArrayPrefix() + "rng");
    }

    // Add pointers to variables
    const NeuronModels::Base *nm = getArchetype().getNeuronModel();
    gen.addVars(nm->getVars(), backend.getArrayPrefix());

    // Extra global parameters are not required for init
    if(!init) {
        gen.addEGPs(nm->getExtraGlobalParams(), backend.getArrayPrefix());

        // Add heterogeneous neuron model parameters
        gen.addHeterogeneousParams(getArchetype().getNeuronModel()->getParamNames(),
                                   [](const NeuronGroupInternal &ng) { return ng.getParams(); },
                                   &NeuronGroupMergedBase::isParamHeterogeneous);

        // Add heterogeneous neuron model derived parameters
        gen.addHeterogeneousDerivedParams(getArchetype().getNeuronModel()->getDerivedParams(),
                                          [](const NeuronGroupInternal &ng) { return ng.getDerivedParams(); },
                                          &NeuronGroupMergedBase::isDerivedParamHeterogeneous);
    }

    // Build vector of vectors of neuron group's merged in syns
    // Loop through merged synaptic inputs in archetypical neuron group
    for(size_t i = 0; i < getArchetype().getMergedInSyn().size(); i++) {
        const SynapseGroupInternal *sg = getArchetype().getMergedInSyn()[i].first;

        // Add pointer to insyn
        addMergedInSynPointerField(gen, precision, "inSynInSyn", i, backend.getArrayPrefix() + "inSyn");

        // Add pointer to dendritic delay buffer if required
        if(sg->isDendriticDelayRequired()) {
            addMergedInSynPointerField(gen, precision, "denDelayInSyn", i, backend.getArrayPrefix() + "denDelay");

            gen.addField("volatile unsigned int*", "denDelayPtrInSyn" + std::to_string(i),
                         [&backend, i, this](const NeuronGroupInternal &, size_t groupIndex)
                         {
                             const std::string &targetName = getSortedMergedInSyns()[groupIndex][i].first->getPSModelTargetName();
                             return "getSymbolAddress(" + backend.getScalarPrefix() + "denDelayPtr" + targetName + ")";
                         });
        }

        // Add pointers to state variables
        if(sg->getMatrixType() & SynapseMatrixWeight::INDIVIDUAL_PSM) {
            for(const auto &v : sg->getPSModel()->getVars()) {
                addMergedInSynPointerField(gen, v.type, v.name + "InSyn", i, backend.getArrayPrefix() + v.name);
            }
        }

        if(!init) {
            /*for(const auto &e : egps) {
                gen.addField(e.type + " " + e.name + std::to_string(i),
                             [e](const typename T::GroupInternal &g){ return e.name + g.getName(); });
            }*/
        }
    }

    // Loop through current sources in archetypical neuron group
    for(size_t i = 0; i < getArchetype().getCurrentSources().size(); i++) {
        const auto *cs = getArchetype().getCurrentSources()[i];

        const auto paramNames = cs->getCurrentSourceModel()->getParamNames();
        for(size_t p = 0; p < paramNames.size(); p++) {
            if(isCurrentSourceParamHeterogeneous(i, p)) {
                gen.addScalarField(paramNames[p] + "CS" + std::to_string(i),
                                   [i, p, this](const NeuronGroupInternal &, size_t groupIndex)
                                   {
                                       const double val = getSortedCurrentSources().at(groupIndex).at(i)->getParams().at(p);
                                       return Utils::writePreciseString(val);
                                   });
            }
        }

        const auto derivedParams = cs->getCurrentSourceModel()->getDerivedParams();
        for(size_t p = 0; p < derivedParams.size(); p++) {
            if(isCurrentSourceDerivedParamHeterogeneous(i, p)) {
                gen.addScalarField(derivedParams[p].name + "CS" + std::to_string(i),
                                   [i, p, this](const NeuronGroupInternal &, size_t groupIndex)
                                   {
                                       const double val = getSortedCurrentSources().at(groupIndex).at(i)->getDerivedParams().at(p);
                                       return Utils::writePreciseString(val);
                                   });
            }
        }

        for(const auto &v : cs->getCurrentSourceModel()->getVars()) {
            assert(!Utils::isTypePointer(v.type));
            gen.addField(v.type + "*", v.name + "CS" + std::to_string(i),
                         [&backend, i, v, this](const NeuronGroupInternal &, size_t groupIndex)
                         {
                             return backend.getArrayPrefix() + v.name + m_SortedCurrentSources.at(groupIndex).at(i)->getName();
                         });
        }

        if(!init) {
            const auto egps = cs->getCurrentSourceModel()->getExtraGlobalParams();
            for(const auto &e : egps) {
                gen.addField(e.type, e.name + "CS" + std::to_string(i),
                             [i, e, &backend, this](const NeuronGroupInternal &, size_t groupIndex)
                             {
                                 return backend.getArrayPrefix() + e.name + getSortedCurrentSources().at(groupIndex).at(i)->getName();
                             },
                             Utils::isTypePointer(e.type) ? decltype(gen)::FieldType::PointerEGP : decltype(gen)::FieldType::ScalarEGP);
            }
        }
    }

    // Loop through incoming synapse groups with postsynaptic update code
    const auto inSynWithPostCode = getArchetype().getInSynWithPostCode();
    for(size_t i = 0; i < inSynWithPostCode.size(); i++) {
        const auto *sg = inSynWithPostCode[i];

        for(const auto &v : sg->getWUModel()->getPostVars()) {
            assert(!Utils::isTypePointer(v.type));
            gen.addField(v.type + "*", v.name + "WUPost" + std::to_string(i),
                         [i, v, &backend, this](const NeuronGroupInternal &, size_t groupIndex)
                         {
                             return backend.getArrayPrefix() + v.name + m_SortedInSynWithPostCode.at(groupIndex).at(i)->getName();
                         });
        }
    }

    // Loop through outgoing synapse groups with presynaptic update code
    const auto outSynWithPreCode = getArchetype().getOutSynWithPreCode();
    for(size_t i = 0; i < outSynWithPreCode.size(); i++) {
        const auto *sg = outSynWithPreCode[i];

        for(const auto &v : sg->getWUModel()->getPreVars()) {
            assert(!Utils::isTypePointer(v.type));
            gen.addField(v.type + "*", v.name + "WUPre" + std::to_string(i),
                         [i, v, &backend, this](const NeuronGroupInternal &, size_t groupIndex)
                         {
                             return backend.getArrayPrefix() + v.name + m_SortedInSynWithPostCode.at(groupIndex).at(i)->getName();
                         });
        }
    }

    std::vector<std::vector<SynapseGroupInternal *>> eventThresholdSGs;

    // Loop through neuron groups
    for(const auto &g : getGroups()) {
        // Reserve vector for this group's children
        eventThresholdSGs.emplace_back();

        // Add synapse groups 
        for(const auto &s : g.get().getSpikeEventCondition()) {
            if(s.egpInThresholdCode) {
                eventThresholdSGs.back().push_back(s.synapseGroup);
            }
        }
    }

    size_t i = 0;
    for(const auto &s : getArchetype().getSpikeEventCondition()) {
        if(s.egpInThresholdCode) {
            const auto sgEGPs = s.synapseGroup->getWUModel()->getExtraGlobalParams();
            for(const auto &egp : sgEGPs) {
                gen.addField(egp.type, egp.name + "EventThresh" + std::to_string(i),
                             [&eventThresholdSGs, &backend, egp, i](const NeuronGroupInternal &, size_t groupIndex)
                             {
                                 return backend.getArrayPrefix() + egp.name + eventThresholdSGs.at(groupIndex).at(i)->getName();
                             },
                             Utils::isTypePointer(egp.type) ? decltype(gen)::FieldType::PointerEGP : decltype(gen)::FieldType::ScalarEGP);
            }
            i++;
        }
    }

    // Generate structure definitions and instantiation
    gen.generate(backend, definitionsInternal, definitionsInternalFunc, definitionsInternalVar, runnerVarDecl, runnerMergedStructAlloc,
                 mergedStructData, init ? "NeuronInit" : "NeuronUpdate");
}
//----------------------------------------------------------------------------
void CodeGenerator::NeuronGroupMergedBase::addMergedInSynPointerField(MergedStructGenerator<NeuronGroupMergedBase> &gen,
                                                                      const std::string &type, const std::string &name, 
                                                                      size_t archetypeIndex, const std::string &prefix) const
{
    assert(!Utils::isTypePointer(type));
    gen.addField(type + "*", name + std::to_string(archetypeIndex),
                 [prefix, archetypeIndex, this](const NeuronGroupInternal &, size_t groupIndex)
                 {
                     return prefix + m_SortedMergedInSyns.at(groupIndex).at(archetypeIndex).first->getPSModelTargetName();
                 });
}

//----------------------------------------------------------------------------
// CodeGenerator::NeuronUpdateGroupMerged
//----------------------------------------------------------------------------
CodeGenerator::NeuronUpdateGroupMerged::NeuronUpdateGroupMerged(size_t index, const std::vector<std::reference_wrapper<const NeuronGroupInternal>> &groups)
    : NeuronGroupMergedBase(index, false, groups)
{

}
//----------------------------------------------------------------------------
std::string CodeGenerator::NeuronUpdateGroupMerged::getCurrentQueueOffset() const
{
    assert(getArchetype().isDelayRequired());
    return "(*group.spkQuePtr * group.numNeurons)";
}
//----------------------------------------------------------------------------
std::string CodeGenerator::NeuronUpdateGroupMerged::getPrevQueueOffset() const
{
    assert(getArchetype().isDelayRequired());
    return "(((*group.spkQuePtr + " + std::to_string(getArchetype().getNumDelaySlots() - 1) + ") % " + std::to_string(getArchetype().getNumDelaySlots()) + ") * group.numNeurons)";
}
//----------------------------------------------------------------------------
void CodeGenerator::NeuronUpdateGroupMerged::generate(const BackendBase &backend, CodeStream &definitionsInternal,
                                                      CodeStream &definitionsInternalFunc, CodeStream &definitionsInternalVar,
                                                      CodeStream &runnerVarDecl, CodeStream &runnerMergedStructAlloc,
                                                      MergedStructData &mergedStructData, const std::string &precision,
                                                      const std::string &timePrecision) const
{
    NeuronGroupMergedBase::generate(backend, definitionsInternal, definitionsInternalFunc, definitionsInternalVar,
                                    runnerVarDecl, runnerMergedStructAlloc, mergedStructData, precision, timePrecision, false);
}

//----------------------------------------------------------------------------
// CodeGenerator::NeuronInitGroupMerged
//----------------------------------------------------------------------------
CodeGenerator::NeuronInitGroupMerged::NeuronInitGroupMerged(size_t index, const std::vector<std::reference_wrapper<const NeuronGroupInternal>> &groups)
    : NeuronGroupMergedBase(index, true, groups)
{

}
//----------------------------------------------------------------------------
void CodeGenerator::NeuronInitGroupMerged::generate(const BackendBase &backend, CodeStream &definitionsInternal,
                                                    CodeStream &definitionsInternalFunc, CodeStream &definitionsInternalVar,
                                                    CodeStream &runnerVarDecl, CodeStream &runnerMergedStructAlloc,
                                                    MergedStructData &mergedStructData, const std::string &precision,
                                                    const std::string &timePrecision) const
{
    NeuronGroupMergedBase::generate(backend, definitionsInternal, definitionsInternalFunc, definitionsInternalVar,
                                    runnerVarDecl, runnerMergedStructAlloc, mergedStructData, precision, timePrecision, true);
}



//----------------------------------------------------------------------------
// CodeGenerator::SynapseDendriticDelayUpdateMergedGroup
//----------------------------------------------------------------------------
CodeGenerator::SynapseDendriticDelayUpdateMergedGroup::SynapseDendriticDelayUpdateMergedGroup(size_t index, const std::vector<std::reference_wrapper<const SynapseGroupInternal>> &groups)
:   GroupMerged<SynapseGroupInternal>(index, groups)
{
}
//----------------------------------------------------------------------------
void CodeGenerator::SynapseDendriticDelayUpdateMergedGroup::generate(const BackendBase &backend, CodeStream &definitionsInternal,
                                                                     CodeStream &definitionsInternalFunc, CodeStream &definitionsInternalVar,
                                                                     CodeStream &runnerVarDecl, CodeStream &runnerMergedStructAlloc,
                                                                     MergedStructData &mergedStructData, const std::string &precision) const
{
    MergedStructGenerator<SynapseDendriticDelayUpdateMergedGroup> gen(*this, precision);

    gen.addField("volatile unsigned int*", "denDelayPtr",
                 [&backend](const SynapseGroupInternal &sg, size_t)
                 {
                     return "getSymbolAddress(" + backend.getScalarPrefix() + "denDelayPtr" + sg.getPSModelTargetName() + ")";
                 });

    // Generate structure definitions and instantiation
    gen.generate(backend, definitionsInternal, definitionsInternalFunc, definitionsInternalVar, runnerVarDecl, runnerMergedStructAlloc,
                 mergedStructData, "SynapseDendriticDelayUpdate");
}

// ----------------------------------------------------------------------------
// SynapseConnectivityHostInitMergedGroup
//----------------------------------------------------------------------------
CodeGenerator::SynapseConnectivityHostInitMergedGroup::SynapseConnectivityHostInitMergedGroup(size_t index, const std::vector<std::reference_wrapper<const SynapseGroupInternal>> &groups)
: GroupMerged<SynapseGroupInternal>(index, groups)
{
}
//------------------------------------------------------------------------
void CodeGenerator::SynapseConnectivityHostInitMergedGroup::generate(const BackendBase &backend, CodeStream &definitionsInternal,
                                                                     CodeStream &definitionsInternalFunc, CodeStream &definitionsInternalVar,
                                                                     CodeStream &runnerVarDecl, CodeStream &runnerMergedStructAlloc,
                                                                     MergedStructData &mergedStructData, const std::string &precision) const
{
    MergedStructGenerator<SynapseConnectivityHostInitMergedGroup> gen(*this, precision);

    // **TODO** these could be generic
    gen.addField("unsigned int", "numSrcNeurons",
                 [](const SynapseGroupInternal &sg, size_t) { return std::to_string(sg.getSrcNeuronGroup()->getNumNeurons()); });
    gen.addField("unsigned int", "numTrgNeurons",
                 [](const SynapseGroupInternal &sg, size_t) { return std::to_string(sg.getTrgNeuronGroup()->getNumNeurons()); });
    gen.addField("unsigned int", "rowStride",
                 [&backend](const SynapseGroupInternal &sg, size_t) { return std::to_string(backend.getSynapticMatrixRowStride(sg)); });

    // Add heterogeneous connectivity initialiser model parameters
    // **TODO** shouldn't non-host have this too!?
    gen.addHeterogeneousParams(getArchetype().getConnectivityInitialiser().getSnippet()->getParamNames(),
                               [](const SynapseGroupInternal &sg) { return sg.getConnectivityInitialiser().getParams(); },
                               &SynapseConnectivityHostInitMergedGroup::isConnectivityHostInitParamHeterogeneous);


    // Add heterogeneous connectivity initialiser derived parameters
    gen.addHeterogeneousDerivedParams(getArchetype().getConnectivityInitialiser().getSnippet()->getDerivedParams(),
                                      [](const SynapseGroupInternal &sg) { return sg.getConnectivityInitialiser().getDerivedParams(); },
                                      &SynapseConnectivityHostInitMergedGroup::isConnectivityHostInitDerivedParamHeterogeneous);

    // Add EGP pointers to struct for both host and device EGPs
    gen.addEGPPointers(getArchetype().getConnectivityInitialiser().getSnippet()->getExtraGlobalParams(), "");
    gen.addEGPPointers(getArchetype().getConnectivityInitialiser().getSnippet()->getExtraGlobalParams(),
                       backend.getArrayPrefix());

    // Generate structure definitions and instantiation
    gen.generate(backend, definitionsInternal, definitionsInternalFunc, definitionsInternalVar, runnerVarDecl, runnerMergedStructAlloc,
                 mergedStructData,  "SynapseConnectivityHostInit", true);
}
//----------------------------------------------------------------------------
bool CodeGenerator::SynapseConnectivityHostInitMergedGroup::isConnectivityHostInitParamHeterogeneous(size_t paramIndex) const
{
    // If parameter isn't referenced in code, there's no point implementing it hetereogeneously!
    const auto *connectInitSnippet = getArchetype().getConnectivityInitialiser().getSnippet();

    // If none of the connection init EGP initiation code references this parameter, return false
    const auto connectInitEGPs = connectInitSnippet->getExtraGlobalParams();
    const std::string paramName = connectInitSnippet->getParamNames().at(paramIndex);
    if(connectInitSnippet->getHostInitCode().find("$(" + paramName + ")") == std::string::npos) {
        return false;
    }
    // Otherwise, return whether values across all groups are heterogeneous
    else {
        return CodeGenerator::GroupMerged<SynapseGroupInternal>::isParamValueHeterogeneous(
            paramIndex,
            [](const SynapseGroupInternal &sg)
            {
                return sg.getConnectivityInitialiser().getParams();
            });
    }
}
//----------------------------------------------------------------------------
bool CodeGenerator::SynapseConnectivityHostInitMergedGroup::isConnectivityHostInitDerivedParamHeterogeneous(size_t paramIndex) const
{
    // If parameter isn't referenced in code, there's no point implementing it hetereogeneously!
    const auto *connectInitSnippet = getArchetype().getConnectivityInitialiser().getSnippet();

    // If none of the connection init EGP initiation code references this parameter, return false
    const auto connectInitEGPs = connectInitSnippet->getExtraGlobalParams();
    const std::string derivedParamName = connectInitSnippet->getDerivedParams().at(paramIndex).name;
    if(connectInitSnippet->getHostInitCode().find("$(" + derivedParamName + ")") == std::string::npos) {
        return false;
    }
    // Otherwise, return whether values across all groups are heterogeneous
    else {
        return CodeGenerator::GroupMerged<SynapseGroupInternal>::isParamValueHeterogeneous(
            paramIndex,
            [](const SynapseGroupInternal &sg)
            {
                return sg.getConnectivityInitialiser().getDerivedParams();
            });
    }
}

// ----------------------------------------------------------------------------
// SynapseConnectivityInitMergedGroup
//----------------------------------------------------------------------------
CodeGenerator::SynapseConnectivityInitMergedGroup::SynapseConnectivityInitMergedGroup(size_t index, const std::vector<std::reference_wrapper<const SynapseGroupInternal>> &groups)
    : GroupMerged<SynapseGroupInternal>(index, groups)
{
}
//------------------------------------------------------------------------
void CodeGenerator::SynapseConnectivityInitMergedGroup::generate(const BackendBase &backend, CodeStream &definitionsInternal,
                                                                 CodeStream &definitionsInternalFunc, CodeStream &definitionsInternalVar,
                                                                 CodeStream &runnerVarDecl, CodeStream &runnerMergedStructAlloc,
                                                                 MergedStructData &mergedStructData, const std::string &precision) const
{
    MergedStructGenerator<SynapseConnectivityInitMergedGroup> gen(*this, precision);

    // **TODO** these could be generic
    gen.addField("unsigned int", "numSrcNeurons",
                 [](const SynapseGroupInternal &sg, size_t) { return std::to_string(sg.getSrcNeuronGroup()->getNumNeurons()); });
    gen.addField("unsigned int", "numTrgNeurons",
                 [](const SynapseGroupInternal &sg, size_t) { return std::to_string(sg.getTrgNeuronGroup()->getNumNeurons()); });
    gen.addField("unsigned int", "rowStride",
                 [&backend](const SynapseGroupInternal &sg, size_t) { return std::to_string(backend.getSynapticMatrixRowStride(sg)); });

    if(getArchetype().getMatrixType() & SynapseMatrixConnectivity::SPARSE) {
        gen.addPointerField("unsigned int", "rowLength", backend.getArrayPrefix() + "rowLength");
        gen.addPointerField(getArchetype().getSparseIndType(), "ind", backend.getArrayPrefix() + "ind");
    }
    else if(getArchetype().getMatrixType() & SynapseMatrixConnectivity::BITMASK) {
        gen.addPointerField("uint32_t", "gp", backend.getArrayPrefix() + "gp");
    }

    // Add EGPs to struct
    gen.addEGPs(getArchetype().getConnectivityInitialiser().getSnippet()->getExtraGlobalParams(),
                backend.getArrayPrefix());

    // Generate structure definitions and instantiation
    gen.generate(backend, definitionsInternal, definitionsInternalFunc, definitionsInternalVar, runnerVarDecl, runnerMergedStructAlloc,
                 mergedStructData, "SynapseConnectivityInit");
}

//----------------------------------------------------------------------------
// CodeGenerator::SynapseGroupMergedBase
//----------------------------------------------------------------------------
std::string CodeGenerator::SynapseGroupMergedBase::getPresynapticAxonalDelaySlot() const
{
    assert(getArchetype().getSrcNeuronGroup()->isDelayRequired());

    const unsigned int numDelaySteps = getArchetype().getDelaySteps();
    if(numDelaySteps == 0) {
        return "(*group.srcSpkQuePtr)";
    }
    else {
        const unsigned int numSrcDelaySlots = getArchetype().getSrcNeuronGroup()->getNumDelaySlots();
        return "((*group.srcSpkQuePtr + " + std::to_string(numSrcDelaySlots - numDelaySteps) + ") % " + std::to_string(numSrcDelaySlots) + ")";
    }
}
//----------------------------------------------------------------------------
std::string CodeGenerator::SynapseGroupMergedBase::getPostsynapticBackPropDelaySlot() const
{
    assert(getArchetype().getTrgNeuronGroup()->isDelayRequired());

    const unsigned int numBackPropDelaySteps = getArchetype().getBackPropDelaySteps();
    if(numBackPropDelaySteps == 0) {
        return "(*group.trgSpkQuePtr)";
    }
    else {
        const unsigned int numTrgDelaySlots = getArchetype().getTrgNeuronGroup()->getNumDelaySlots();
        return "((*group.trgSpkQuePtr + " + std::to_string(numTrgDelaySlots - numBackPropDelaySteps) + ") % " + std::to_string(numTrgDelaySlots) + ")";
    }
}
//----------------------------------------------------------------------------
std::string CodeGenerator::SynapseGroupMergedBase::getDendriticDelayOffset(const std::string &offset) const
{
    assert(getArchetype().isDendriticDelayRequired());

    if(offset.empty()) {
        return "(*group.denDelayPtr * group.numTrgNeurons) + ";
    }
    else {
        return "(((*group.denDelayPtr + " + offset + ") % " + std::to_string(getArchetype().getMaxDendriticDelayTimesteps()) + ") * group.numTrgNeurons) + ";
    }
}
//----------------------------------------------------------------------------
bool CodeGenerator::SynapseGroupMergedBase::isWUVarInitParamHeterogeneous(size_t varIndex, size_t paramIndex) const
{
    // If parameter isn't referenced in code, there's no point implementing it hetereogeneously!
    const auto *varInitSnippet = getArchetype().getWUVarInitialisers().at(varIndex).getSnippet();
    const std::string paramName = varInitSnippet->getParamNames().at(paramIndex);
    if(varInitSnippet->getCode().find("$(" + paramName + ")") == std::string::npos) {
        return false;
    }
    // Otherwise, return whether values across all groups are heterogeneous
    else {
        return CodeGenerator::GroupMerged<SynapseGroupInternal>::isParamValueHeterogeneous(
            paramIndex,
            [varIndex](const SynapseGroupInternal &sg)
            {
                return sg.getWUVarInitialisers().at(varIndex).getParams();
            });
    }
}
//----------------------------------------------------------------------------
bool CodeGenerator::SynapseGroupMergedBase::isWUVarInitDerivedParamHeterogeneous(size_t varIndex, size_t paramIndex) const
{
    // If derived parameter isn't referenced in code, there's no point implementing it hetereogeneously!
    const auto *varInitSnippet = getArchetype().getWUVarInitialisers().at(varIndex).getSnippet();
    const std::string derivedParamName = varInitSnippet->getDerivedParams().at(paramIndex).name;
    if(varInitSnippet->getCode().find("$(" + derivedParamName + ")") == std::string::npos) {
        return false;
    }
    // Otherwise, return whether values across all groups are heterogeneous
    else {
        return CodeGenerator::GroupMerged<SynapseGroupInternal>::isParamValueHeterogeneous(
            paramIndex,
            [varIndex](const SynapseGroupInternal &sg)
            {
                return sg.getWUVarInitialisers().at(varIndex).getDerivedParams();
            });
    }
}
//----------------------------------------------------------------------------
void CodeGenerator::SynapseGroupMergedBase::generate(const BackendBase &backend, CodeStream &definitionsInternal,
                                                     CodeStream &definitionsInternalFunc, CodeStream &definitionsInternalVar,
                                                     CodeStream &runnerVarDecl, CodeStream &runnerMergedStructAlloc,
                                                     MergedStructData &mergedStructData, const std::string &precision, 
                                                     const std::string &timePrecision, const std::string &name, Role role) const
{
    const bool updateRole = ((role == Role::PresynapticUpdate)
                             || (role == Role::PostsynapticUpdate)
                             || (role == Role::SynapseDynamics));
    const WeightUpdateModels::Base *wum = getArchetype().getWUModel();

    MergedStructGenerator<SynapseGroupMergedBase> gen(*this, precision);

    gen.addField("unsigned int", "rowStride",
                 [&backend](const SynapseGroupInternal &sg, size_t) { return std::to_string(backend.getSynapticMatrixRowStride(sg)); });
    if(role == Role::PostsynapticUpdate || role == Role::SparseInit) {
        gen.addField("unsigned int", "colStride",
                     [](const SynapseGroupInternal &sg, size_t) { return std::to_string(sg.getMaxSourceConnections()); });
    }

    gen.addField("unsigned int", "numSrcNeurons",
                 [](const SynapseGroupInternal &sg, size_t) { return std::to_string(sg.getSrcNeuronGroup()->getNumNeurons()); });
    gen.addField("unsigned int", "numTrgNeurons",
                 [](const SynapseGroupInternal &sg, size_t) { return std::to_string(sg.getTrgNeuronGroup()->getNumNeurons()); });

    // If this role is one where postsynaptic input can be provided
    if(role == Role::PresynapticUpdate || role == Role::SynapseDynamics) {
        if(getArchetype().isDendriticDelayRequired()) {
            addPSPointerField(gen, precision, "denDelay", backend.getArrayPrefix() + "denDelay");
            gen.addField("volatile unsigned int*", "denDelayPtr",
                         [&backend](const SynapseGroupInternal &sg, size_t)
                         {
                             return "getSymbolAddress(" + backend.getScalarPrefix() + "denDelayPtr" + sg.getPSModelTargetName() + ")";
                         });
        }
        else {
            addPSPointerField(gen, precision, "inSyn", backend.getArrayPrefix() + "inSyn");
        }
    }

    if(role == Role::PresynapticUpdate) {
        if(getArchetype().isTrueSpikeRequired()) {
            addSrcPointerField(gen, "unsigned int", "srcSpkCnt", backend.getArrayPrefix() + "glbSpkCnt");
            addSrcPointerField(gen, "unsigned int", "srcSpk", backend.getArrayPrefix() + "glbSpk");
        }

        if(getArchetype().isSpikeEventRequired()) {
            addSrcPointerField(gen, "unsigned int", "srcSpkCntEvnt", backend.getArrayPrefix() + "glbSpkCntEvnt");
            addSrcPointerField(gen, "unsigned int", "srcSpkEvnt", backend.getArrayPrefix() + "glbSpkEvnt");
        }
    }
    else if(role == Role::PostsynapticUpdate) {
        addTrgPointerField(gen, "unsigned int", "trgSpkCnt", backend.getArrayPrefix() + "glbSpkCnt");
        addTrgPointerField(gen, "unsigned int", "trgSpk", backend.getArrayPrefix() + "glbSpk");
    }

    // If this structure is used for updating rather than initializing
    if(updateRole) {
        // If presynaptic population has delay buffers
        if(getArchetype().getSrcNeuronGroup()->isDelayRequired()) {
            gen.addField("volatile unsigned int*", "srcSpkQuePtr",
                         [&backend](const SynapseGroupInternal &sg, size_t)
                         {
                             return "getSymbolAddress(" + backend.getScalarPrefix() + "spkQuePtr" + sg.getSrcNeuronGroup()->getName() + ")";
                         });
        }

        // If postsynaptic population has delay buffers
        if(getArchetype().getTrgNeuronGroup()->isDelayRequired()) {
            gen.addField("volatile unsigned int*", "trgSpkQuePtr",
                         [&backend](const SynapseGroupInternal &sg, size_t)
                         {
                             return "getSymbolAddress(" + backend.getScalarPrefix() + "spkQuePtr" + sg.getTrgNeuronGroup()->getName() + ")";
                         });
        }

        // Get correct code string
        // **NOTE** we concatenate sim code and event code so both get tested
        const std::string code = ((role == Role::PresynapticUpdate) ? (wum->getSimCode() + wum->getEventCode())
                                  : (role == Role::PostsynapticUpdate) ? wum->getLearnPostCode() : wum->getSynapseDynamicsCode());

        // Loop through variables in presynaptic neuron model
        const auto preVars = getArchetype().getSrcNeuronGroup()->getNeuronModel()->getVars();
        for(const auto &v : preVars) {
            // If variable is referenced in code string, add source pointer
            if(code.find("$(" + v.name + "_pre)") != std::string::npos) {
                addSrcPointerField(gen, v.type, v.name + "Pre", backend.getArrayPrefix() + v.name);
            }
        }

        // Loop through variables in postsynaptic neuron model
        const auto postVars = getArchetype().getTrgNeuronGroup()->getNeuronModel()->getVars();
        for(const auto &v : postVars) {
            // If variable is referenced in code string, add target pointer
            if(code.find("$(" + v.name + "_post)") != std::string::npos) {
                addTrgPointerField(gen, v.type, v.name + "Post", backend.getArrayPrefix() + v.name);
            }
        }

        // Loop through extra global parameters in presynaptic neuron model
        const auto preEGPs = getArchetype().getSrcNeuronGroup()->getNeuronModel()->getExtraGlobalParams();
        for(const auto &e : preEGPs) {
            if(code.find("$(" + e.name + "_pre)") != std::string::npos) {
                gen.addField(e.type, e.name + "Pre",
                             [e](const SynapseGroupInternal &sg, size_t) { return e.name + sg.getSrcNeuronGroup()->getName(); },
                             Utils::isTypePointer(e.type) ? decltype(gen)::FieldType::PointerEGP : decltype(gen)::FieldType::ScalarEGP);
            }
        }

        // Loop through extra global parameters in postsynaptic neuron model
        const auto postEGPs = getArchetype().getTrgNeuronGroup()->getNeuronModel()->getExtraGlobalParams();
        for(const auto &e : postEGPs) {
            if(code.find("$(" + e.name + "_post)") != std::string::npos) {
                gen.addField(e.type, e.name + "Post",
                             [e](const SynapseGroupInternal &sg, size_t) { return e.name + sg.getTrgNeuronGroup()->getName(); },
                             Utils::isTypePointer(e.type) ? decltype(gen)::FieldType::PointerEGP : decltype(gen)::FieldType::ScalarEGP);
            }
        }

        // Add spike times if required
        if(wum->isPreSpikeTimeRequired()) {
            addSrcPointerField(gen, timePrecision, "sTPre", backend.getArrayPrefix() + "sT");
        }
        if(wum->isPostSpikeTimeRequired()) {
            addTrgPointerField(gen, timePrecision, "sTPost", backend.getArrayPrefix() + "sT");
        }

        // Add pre and postsynaptic variables to struct
        gen.addVars(wum->getPreVars(), backend.getArrayPrefix());
        gen.addVars(wum->getPostVars(), backend.getArrayPrefix());

        // Add EGPs to struct
        gen.addEGPs(wum->getExtraGlobalParams(), backend.getArrayPrefix());
    }

    // Add pointers to connectivity data
    if(getArchetype().getMatrixType() & SynapseMatrixConnectivity::SPARSE) {
        gen.addPointerField("unsigned int", "rowLength", backend.getArrayPrefix() + "rowLength");
        gen.addPointerField(getArchetype().getSparseIndType(), "ind", backend.getArrayPrefix() + "ind");

        // Add additional structure for postsynaptic access
        if(backend.isPostsynapticRemapRequired() && !wum->getLearnPostCode().empty()
           && (role == Role::PostsynapticUpdate || role == Role::SparseInit))
        {
            gen.addPointerField("unsigned int", "colLength", backend.getArrayPrefix() + "colLength");
            gen.addPointerField("unsigned int", "remap", backend.getArrayPrefix() + "remap");
        }

        // Add additional structure for synapse dynamics access
        if(backend.isSynRemapRequired() && !wum->getSynapseDynamicsCode().empty()
           && (role == Role::SynapseDynamics || role == Role::SparseInit))
        {
            gen.addPointerField("unsigned int", "synRemap", backend.getArrayPrefix() + "synRemap");
        }
    }
    else if(getArchetype().getMatrixType() & SynapseMatrixConnectivity::BITMASK) {
        gen.addPointerField("uint32_t", "gp", backend.getArrayPrefix() + "gp");
    }
    else if(getArchetype().getMatrixType() & SynapseMatrixConnectivity::PROCEDURAL) {
        gen.addEGPs(getArchetype().getConnectivityInitialiser().getSnippet()->getExtraGlobalParams(),
                    backend.getArrayPrefix());
    }

    // If synaptic matrix weights are individual, add pointers to var pointers to struct
    if(getArchetype().getMatrixType() & SynapseMatrixWeight::INDIVIDUAL) {
        gen.addVars(wum->getVars(), backend.getArrayPrefix());
    }
    // If synaptic matrix weights are procedural or we are initializing
    if(getArchetype().getMatrixType() & SynapseMatrixWeight::PROCEDURAL || !updateRole) {
        gen.addHeterogeneousVarInitParams(wum->getVars(), &SynapseGroupInternal::getWUVarInitialisers,
                                          &SynapseGroupMergedBase::isWUVarInitParamHeterogeneous);

        gen.addHeterogeneousVarInitDerivedParams(wum->getVars(), &SynapseGroupInternal::getWUVarInitialisers,
                                                 &SynapseGroupMergedBase::isWUVarInitDerivedParamHeterogeneous);
    }

    // Generate structure definitions and instantiation
    gen.generate(backend, definitionsInternal, definitionsInternalFunc, definitionsInternalVar, runnerVarDecl, runnerMergedStructAlloc,
                 mergedStructData, name);
}
//----------------------------------------------------------------------------
void CodeGenerator::SynapseGroupMergedBase::addPSPointerField(MergedStructGenerator<SynapseGroupMergedBase> &gen,
                                                              const std::string &type, const std::string &name, const std::string &prefix) const
{
    assert(!Utils::isTypePointer(type));
    gen.addField(type + "*", name, [prefix](const SynapseGroupInternal &sg, size_t) { return prefix + sg.getPSModelTargetName(); });
}
//----------------------------------------------------------------------------
void CodeGenerator::SynapseGroupMergedBase::addSrcPointerField(MergedStructGenerator<SynapseGroupMergedBase> &gen,
                                                               const std::string &type, const std::string &name, const std::string &prefix) const
{
    assert(!Utils::isTypePointer(type));
    gen.addField(type + "*", name, [prefix](const SynapseGroupInternal &sg, size_t) { return prefix + sg.getSrcNeuronGroup()->getName(); });
}
//----------------------------------------------------------------------------
void CodeGenerator::SynapseGroupMergedBase::addTrgPointerField(MergedStructGenerator<SynapseGroupMergedBase> &gen,
                                                               const std::string &type, const std::string &name, const std::string &prefix) const
{
    assert(!Utils::isTypePointer(type));
    gen.addField(type + "*", name, [prefix](const SynapseGroupInternal &sg, size_t) { return prefix + sg.getTrgNeuronGroup()->getName(); });
}