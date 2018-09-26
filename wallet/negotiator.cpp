// Copyright 2018 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "negotiator.h"
#include "core/block_crypt.h"

namespace beam { namespace wallet
{
    using namespace ECC;
    using namespace std;

    BaseTransaction::BaseTransaction(INegotiatorGateway& gateway
                                   , beam::IKeyChain::Ptr keychain
                                   , const TxDescription& txDesc)
        : m_gateway{ gateway }
        , m_keychain{ keychain }
        , m_txDesc{ txDesc }
    {
        assert(keychain);
    }

    bool BaseTransaction::getParameter(TxParams paramID, ECC::Point::Native& value)
    {
        ECC::Point pt;
        if (getParameter(paramID, pt))
        {
            return value.Import(pt);
        }
        return false;
    }

    bool BaseTransaction::getParameter(TxParams paramID, ECC::Scalar::Native& value)
    {
        ECC::Scalar s;
        if (getParameter(paramID, s))
        {
             value.Import(s);
             return true;
        }
        return false;
    }

    void BaseTransaction::setParameter(TxParams paramID, const ECC::Point::Native& value)
    {
        ECC::Point pt;
        if (value.Export(pt))
        {
            setParameter(paramID, pt);
        }
    }

    void BaseTransaction::setParameter(TxParams paramID, const ECC::Scalar::Native& value)
    {
        ECC::Scalar s;
        value.Export(s);
        setParameter(paramID, s);
    }

    TxKernel* BaseTransaction::getKernel() const
    {
        if (m_txDesc.m_status == TxDescription::Registered)
        {
            // TODO: what should we do in case when we have more than one kernel
            if (m_kernel)
            {
                return m_kernel.get();
            }
            else if (m_transaction && !m_transaction->m_vKernelsOutput.empty())
            {
                return m_transaction->m_vKernelsOutput[0].get();
            }
        }
        return nullptr;
    }

    bool BaseTransaction::getTip(Block::SystemState::Full& state) const
    {
        return m_gateway.get_tip(state);
    }

    const TxID& BaseTransaction::getTxID() const
    {
        return m_txDesc.m_txId;
    }

    void BaseTransaction::cancel()
    {
        if (m_txDesc.m_status == TxDescription::Pending)
        {
            m_keychain->deleteTx(m_txDesc.m_txId);
        }
        else
        {
            updateTxDescription(TxDescription::Cancelled);
            rollbackTx();
            m_gateway.send_tx_failed(m_txDesc);
        }
    }

    SendTransaction::SendTransaction(INegotiatorGateway& gateway
                                    , beam::IKeyChain::Ptr keychain
                                    , const TxDescription& txDesc)
        : BaseTransaction{ gateway, keychain, txDesc }
    {

    }

    void SendTransaction::update()
    {
        int reason = 0;
        if (getParameter(TxParams::FailureReason, reason))
        {
            onFailed();
            return;
        }
        bool sender = m_txDesc.m_sender;
        Scalar::Native peerOffset;
        bool initiator = !getParameter(TxParams::PeerOffset, peerOffset);
        Scalar::Native offset;
        Scalar::Native blindingExcess;
        if (!getParameter(TxParams::BlindingExcess, blindingExcess)
         || !getParameter(TxParams::Offset, offset))
        {
            LOG_INFO() << m_txDesc.m_txId << (sender ? " Sending " : " Receiving ") << PrintableAmount(m_txDesc.m_amount) << " (fee: " << PrintableAmount(m_txDesc.m_fee) << ")";
            Height currentHeight = m_keychain->getCurrentHeight();
            m_txDesc.m_minHeight = currentHeight;

            if (sender)
            {
                Amount amountWithFee = m_txDesc.m_amount + m_txDesc.m_fee;
                auto coins = m_keychain->selectCoins(amountWithFee);
                if (coins.empty())
                {
                    LOG_ERROR() << "You only have " << PrintableAmount(getAvailable(m_keychain));
                    onFailed(!initiator);
                    return;
                }

                for (auto& coin : coins)
                {
                    blindingExcess += m_keychain->calcKey(coin);
                    coin.m_spentTxId = m_txDesc.m_txId;
                }
                m_keychain->update(coins);

                // calculate change amount and create corresponding output if needed
                Amount change = 0;
                for (const auto &coin : coins)
                {
                    change += coin.m_amount;
                }
                change -= amountWithFee;
                if (change > 0)
                {
                    Coin newUtxo{ change, Coin::Draft, currentHeight };
                    newUtxo.m_createTxId = m_txDesc.m_txId;
                    m_keychain->store(newUtxo);

                    Scalar::Native blindingFactor = m_keychain->calcKey(newUtxo);
                    auto[privateExcess, newOffset] = splitKey(blindingFactor, newUtxo.m_id);

                    blindingFactor = -privateExcess;
                    blindingExcess += blindingFactor;
                    offset += newOffset;

                    m_txDesc.m_change = change;
                }

                auto address = m_keychain->getAddress(m_txDesc.m_peerId);

                if (address.is_initialized() && address->m_own)
                {
                    sendSelfTx();
                    return;
                }
            }
            else
            {
                Coin newUtxo{ m_txDesc.m_amount, Coin::Draft, m_txDesc.m_minHeight };
                newUtxo.m_createTxId = m_txDesc.m_txId;
                m_keychain->store(newUtxo);

                Scalar::Native blindingFactor = m_keychain->calcKey(newUtxo);
                auto[privateExcess, newOffset] = splitKey(blindingFactor, newUtxo.m_id);

                blindingFactor = -privateExcess;
                blindingExcess += blindingFactor;
                offset += newOffset;

                LOG_INFO() << m_txDesc.m_txId << " Invitation accepted";
            }

            setParameter(TxParams::BlindingExcess, blindingExcess);
            setParameter(TxParams::Offset, offset);

            updateTxDescription(TxDescription::InProgress);
        }

        auto kernel = createKernel(m_txDesc.m_fee, m_txDesc.m_minHeight);
        auto msig = createMultiSig(*kernel, blindingExcess);

        Point::Native publicNonce = Context::get().G * msig.m_Nonce;
        Point::Native publicExcess = Context::get().G * blindingExcess;

        Point::Native publicPeerNonce, publicPeerExcess;

        if (!getParameter(TxParams::PublicPeerNonce, publicPeerNonce)
            || !getParameter(TxParams::PublicPeerExcess, publicPeerExcess))
        {
            assert(initiator);
            const TxID& txID = m_txDesc.m_txId;

            Invite inviteMsg;
            inviteMsg.m_txId = txID;
            inviteMsg.m_amount = m_txDesc.m_amount;
            inviteMsg.m_fee = m_txDesc.m_fee;
            inviteMsg.m_height = m_txDesc.m_minHeight;
            inviteMsg.m_send = m_txDesc.m_sender;
            inviteMsg.m_inputs = getTxInputs(txID);
            inviteMsg.m_outputs = getTxOutputs(txID);
            inviteMsg.m_publicPeerExcess = publicExcess;
            inviteMsg.m_publicPeerNonce = publicNonce;
            inviteMsg.m_offset = offset;

            m_gateway.send_tx_invitation(m_txDesc, move(inviteMsg));

            //sendInvitation(publicExcess, publicNonce);
            return;
        }

        msig.m_NoncePub = publicNonce + publicPeerNonce;

        Point::Native totalPublicExcess = publicExcess;
        totalPublicExcess += publicPeerExcess;
        kernel->m_Excess = totalPublicExcess;

        Hash::Value message;
        kernel->get_Hash(message);
        Scalar::Native partialSignature;
        kernel->m_Signature.CoSign(partialSignature, message, blindingExcess, msig);
        LOG_DEBUG() << "Total public excess: " << totalPublicExcess << " PeerExcess: "<< publicPeerExcess << " PublicExcess:" << publicExcess << " Message: " << message << " pubNonce: " << msig.m_NoncePub;
        Scalar::Native peerSignature;
        if (!getParameter(TxParams::PeerSignature, peerSignature))
        {
            // invited participant
            assert(!initiator);
            //sendConfirmInvitation();
            ConfirmInvitation confirmMsg;
            confirmMsg.m_txId = m_txDesc.m_txId;
            confirmMsg.m_publicPeerExcess = publicExcess;
            confirmMsg.m_peerSignature = partialSignature;
            confirmMsg.m_publicPeerNonce = publicNonce;

            m_gateway.send_tx_confirmation(m_txDesc, move(confirmMsg));

            return;
        }

        Signature peerSig;
        peerSig.m_e = kernel->m_Signature.m_e;
        peerSig.m_k = peerSignature;
        if (!peerSig.IsValidPartial(publicPeerNonce, publicPeerExcess))
        {
            onFailed(true);
            return;
        }

        kernel->m_Signature.m_k = partialSignature + peerSignature;

        bool isRegistered = false;
        if (!getParameter(TxParams::TransactionRegistered, isRegistered))
        {
            vector<Input::Ptr> inputs;
            vector<Output::Ptr> outputs;
            if (!getParameter(TxParams::PeerInputs, inputs)
                || !getParameter(TxParams::PeerOutputs, outputs))
            {
                // initiator
                assert(initiator);
                Scalar s;
                partialSignature.Export(s);
                sendConfirmTransaction(s);
            }
            else
            {
                // invited participant
                auto transaction = make_shared<Transaction>();
                transaction->m_vKernelsOutput.push_back(move(kernel));
                transaction->m_Offset = peerOffset + offset;
                transaction->m_vInputs = move(inputs);
                transaction->m_vOutputs = move(outputs);

                {
                    auto inputs = getTxInputs(m_txDesc.m_txId);
                    move(inputs.begin(), inputs.end(), back_inserter(transaction->m_vInputs));

                    auto outputs = getTxOutputs(m_txDesc.m_txId);
                    move(outputs.begin(), outputs.end(), back_inserter(transaction->m_vOutputs));
                }

                transaction->Sort();

                // Verify final transaction
                TxBase::Context ctx;
                if (!transaction->IsValid(ctx))
                {
                    onFailed(true);
                    return;
                }
                m_gateway.register_tx(m_txDesc, transaction);
            }
            return;
        }

        if (!isRegistered)
        {
            onFailed(true);
            return;
        }

        Merkle::Proof kernelProof;
        if (!getParameter(TxParams::KernelProof, kernelProof))
        {
            if (!initiator)
            {
                m_gateway.send_tx_registered(m_txDesc);
            }
            confirmKernel(*kernel);
            return;
        }

        Block::SystemState::Full state;
        if (!getTip(state) || !state.IsValidProofKernel(*kernel, kernelProof))
        {
            if (!m_gateway.isTestMode())
            {
                return;
            }
        }

        //  confirmOutputs();

        completeTx();
    }


    void SendTransaction::sendSelfTx()
    {
        //// Create output UTXOs for main amount
        //createOutputUtxo(m_txDesc.m_amount, m_txDesc.m_minHeight);

        //// Create empty transaction
        //m_transaction = std::make_shared<Transaction>();
        //m_transaction->m_Offset = Zero;

        //// Calculate public key for excess
        //Point::Native excess;
        //if (!excess.Import(getPublicExcess()))
        //{
        //    //onFailed(true);
        //    return;
        //}

        //// Calculate signature
        //Scalar::Native signature = createSignature();

        //// Construct and verify transaction
        //if (!constructTxInternal(signature))
        //{
        //    //onFailed(true);
        //    return;
        //}

        //updateTxDescription(TxDescription::InProgress);
        //sendNewTransaction();
    }

    void SendTransaction::sendInvite() const
    {
        bool sender = m_txDesc.m_sender;
        Height currentHeight = m_txDesc.m_minHeight;
        const TxID& txID = m_txDesc.m_txId;

        Invite inviteMsg;
        inviteMsg.m_txId = txID;
        inviteMsg.m_amount = m_txDesc.m_amount;
        inviteMsg.m_fee = m_txDesc.m_fee;
        inviteMsg.m_height = currentHeight;
        inviteMsg.m_send = sender;
        inviteMsg.m_inputs = getTxInputs(txID);
        inviteMsg.m_outputs = getTxOutputs(txID);
        inviteMsg.m_publicPeerExcess = getPublicExcess();
        inviteMsg.m_publicPeerNonce = getPublicNonce();
        inviteMsg.m_offset = m_offset;

        m_gateway.send_tx_invitation(m_txDesc, move(inviteMsg));
    }

    void SendTransaction::sendConfirmTransaction(const Scalar& peerSignature) const
    {
        ConfirmTransaction confirmMsg;
        confirmMsg.m_txId = m_txDesc.m_txId;
        confirmMsg.m_peerSignature = peerSignature;

        m_gateway.send_tx_confirmation(m_txDesc, move(confirmMsg));
    }

    ReceiveTransaction::ReceiveTransaction(INegotiatorGateway& gateway
                                         , beam::IKeyChain::Ptr keychain
                                         , const TxDescription& txDesc)
        : BaseTransaction{ gateway, keychain, txDesc }
    {

    }


    void ReceiveTransaction::update()
    {
        Scalar::Native blindingExcess;

        vector<Output::Ptr> outputs;
        if (!getParameter(TxParams::Outputs, outputs))
        {
            LOG_INFO() << m_txDesc.m_txId << " Receiving " << PrintableAmount(m_txDesc.m_amount) << " (fee: " << PrintableAmount(m_txDesc.m_fee) << ")";

            Coin newUtxo{ m_txDesc.m_amount, Coin::Draft, m_txDesc.m_minHeight };
            newUtxo.m_createTxId = m_txDesc.m_txId;
            m_keychain->store(newUtxo);

            Scalar::Native blindingFactor = m_keychain->calcKey(newUtxo);
            auto[privateExcess, newOffset] = splitKey(blindingFactor, newUtxo.m_id);

            blindingFactor = -privateExcess;
            blindingExcess += blindingFactor;

            setParameter(TxParams::Outputs, getTxOutputs(getTxID()));
            setParameter(TxParams::BlindingExcess, blindingExcess);
            setParameter(TxParams::Offset, newOffset);
            LOG_INFO() << m_txDesc.m_txId << " Invitation accepted";
            updateTxDescription(TxDescription::InProgress);
        }

        Scalar::Native offset;
        if (!getParameter(TxParams::BlindingExcess, blindingExcess)
            || !getParameter(TxParams::Offset, offset))
        {
            onFailed(true);
            return;
        }

        auto kernel = createKernel(m_txDesc.m_fee, m_txDesc.m_minHeight);
        auto msig = createMultiSig(*kernel, blindingExcess);

        Point::Native publicNonce = Context::get().G * msig.m_Nonce;
        Point::Native publicExcess = Context::get().G * blindingExcess;

        Scalar::Native peerOffset;
        Point::Native publicPeerNonce, publicPeerExcess;

        if (!getParameter(TxParams::PeerOffset, peerOffset)
            || !getParameter(TxParams::PublicPeerNonce, publicPeerNonce)
            || !getParameter(TxParams::PublicPeerExcess, publicPeerExcess))
        {
            onFailed(true);
            return;
        }

        msig.m_NoncePub = publicNonce + publicPeerNonce;

        Point::Native totalPublicExcess = publicExcess;
        totalPublicExcess += publicPeerExcess;
        kernel->m_Excess = totalPublicExcess;

        Hash::Value message;
        kernel->get_Hash(message);
        Scalar::Native partialSignature;
        kernel->m_Signature.CoSign(partialSignature, message, blindingExcess, msig);
        LOG_DEBUG() << "Total public excess: " << totalPublicExcess << " PeerExcess: " << publicPeerExcess << " PublicExcess:" << publicExcess << " Message: " << message << " pubNonce: " << msig.m_NoncePub;
        Scalar::Native peerSignature;
        if (!getParameter(TxParams::PeerSignature, peerSignature))
        {
            //sendConfirmInvitation();

            ConfirmInvitation confirmMsg;
            confirmMsg.m_txId = m_txDesc.m_txId;
            confirmMsg.m_publicPeerExcess = publicExcess;
            confirmMsg.m_peerSignature = partialSignature;
            confirmMsg.m_publicPeerNonce = publicNonce;

            m_gateway.send_tx_confirmation(m_txDesc, move(confirmMsg));

            return;
        }

        Signature peerSig;
        peerSig.m_e = kernel->m_Signature.m_e;
        peerSig.m_k = peerSignature;
        if (!peerSig.IsValidPartial(publicPeerNonce, publicPeerExcess))
        {
            onFailed(true);
            return;
        }

        kernel->m_Signature.m_k = partialSignature + peerSignature;

        bool isRegistered = false;
        if (!getParameter(TxParams::TransactionRegistered, isRegistered))
        {
            auto transaction = make_shared<Transaction>();
            transaction->m_vKernelsOutput.push_back(move(kernel));
            transaction->m_Offset = peerOffset + offset;
            getParameter(TxParams::PeerInputs, transaction->m_vInputs);
            getParameter(TxParams::PeerOutputs, transaction->m_vOutputs);

            {
                auto inputs = getTxInputs(m_txDesc.m_txId);
                move(inputs.begin(), inputs.end(), back_inserter(transaction->m_vInputs));

                auto outputs = getTxOutputs(m_txDesc.m_txId);
                move(outputs.begin(), outputs.end(), back_inserter(transaction->m_vOutputs));
            }

            transaction->Sort();

            // Verify final transaction
            TxBase::Context ctx;
            if (!transaction->IsValid(ctx))
            {
                onFailed(true);
                return;
            }
            m_gateway.register_tx(m_txDesc, transaction);
            return;
        }

        if (!isRegistered)
        {
            onFailed(true);
            return;
        }

        Merkle::Proof kernelProof;
        if (!getParameter(TxParams::KernelProof, kernelProof))
        {
            m_gateway.send_tx_registered(m_txDesc);
            confirmKernel(*kernel);
            return;
        }

        Block::SystemState::Full state;
        if (!getTip(state) || !state.IsValidProofKernel(*kernel, kernelProof))
        {
            if (!m_gateway.isTestMode())
            {
                return;
            }
        }

        //     confirmOutputs();

        completeTx();
    }

    void ReceiveTransaction::sendConfirmInvitation() const
    {
        ConfirmInvitation confirmMsg;
        confirmMsg.m_txId = m_txDesc.m_txId;
        confirmMsg.m_publicPeerExcess = getPublicExcess();
        NoLeak<Scalar> t;
        createSignature2(confirmMsg.m_peerSignature, confirmMsg.m_publicPeerNonce, t.V);

        m_gateway.send_tx_confirmation(m_txDesc, move(confirmMsg));
    }

    bool BaseTransaction::registerTxInternal(const ECC::Scalar& peerSignature)
    {
        if (!isValidSignature(peerSignature))
            return false;

        // Calculate final signature
        Scalar::Native senderSignature;
        senderSignature = peerSignature;
        Scalar::Native receiverSignature = createSignature();
        Scalar::Native finialSignature = senderSignature + receiverSignature;
        return constructTxInternal(finialSignature);
    }

    bool BaseTransaction::constructTxInternal(const Scalar::Native& signature)
    {
        // Create transaction kernel and transaction
        m_kernel->m_Signature.m_k = signature;
        m_transaction = make_shared<Transaction>();
        m_transaction->m_vKernelsOutput.push_back(move(m_kernel));
        m_transaction->m_Offset = m_offset;
        getParameter(TxParams::PeerInputs, m_transaction->m_vInputs);
        getParameter(TxParams::PeerOutputs, m_transaction->m_vOutputs);

        {
            auto inputs = getTxInputs(m_txDesc.m_txId);
            move(inputs.begin(), inputs.end(), back_inserter(m_transaction->m_vInputs));

            auto outputs = getTxOutputs(m_txDesc.m_txId);
            move(outputs.begin(), outputs.end(), back_inserter(m_transaction->m_vOutputs));
        }

        m_transaction->Sort();

        // Verify final transaction
        TxBase::Context ctx;
        return m_transaction->IsValid(ctx);
    }

    void BaseTransaction::sendNewTransaction() const
    {
        m_gateway.register_tx(m_txDesc, m_transaction);
    }

    void BaseTransaction::rollbackTx()
    {
        LOG_INFO() << m_txDesc.m_txId << " Transaction failed. Rollback...";
        m_keychain->rollbackTx(m_txDesc.m_txId);
    }

    void BaseTransaction::confirmKernel(const TxKernel& kernel)
    {
        LOG_INFO() << m_txDesc.m_txId << " Transaction registered";
        updateTxDescription(TxDescription::Registered);

        auto coins = m_keychain->getCoinsCreatedByTx(m_txDesc.m_txId);

        for (auto& coin : coins)
        {
            coin.m_status = Coin::Unconfirmed;
        }
        m_keychain->update(coins);

        m_gateway.confirm_kernel(m_txDesc, kernel);
    }

    void BaseTransaction::confirmOutputs()
    {
        m_gateway.confirm_outputs(m_txDesc);
    }

    void BaseTransaction::completeTx()
    {
        LOG_INFO() << m_txDesc.m_txId << " Transaction completed";
        updateTxDescription(TxDescription::Completed);
        m_gateway.on_tx_completed(m_txDesc);
    }

    void BaseTransaction::updateTxDescription(TxDescription::Status s)
    {
        m_txDesc.m_status = s;
        m_txDesc.m_modifyTime = getTimestamp();
        m_keychain->saveTx(m_txDesc);
    }

    bool BaseTransaction::prepareSenderUtxos(const Height& currentHeight)
    {
        Amount amountWithFee = m_txDesc.m_amount + m_txDesc.m_fee;
        auto coins = m_keychain->selectCoins(amountWithFee);
        if (coins.empty())
        {
            LOG_ERROR() << "You only have " << PrintableAmount(getAvailable(m_keychain));
            return false;
        }
        Scalar::Native blindingFactor;
        for (auto& coin : coins)
        {
            blindingFactor += m_keychain->calcKey(coin);
            coin.m_spentTxId = m_txDesc.m_txId;
        }
        m_keychain->update(coins);
        

        // calculate change amount and create corresponding output if needed
        Amount change = 0;
        for (const auto &coin : coins)
        {
            change += coin.m_amount;
        }
        change -= amountWithFee;
        if (change > 0)
        {
            createOutputUtxo(change, currentHeight);
            m_txDesc.m_change = change;
        }

        setParameter(TxParams::BlindingExcess, blindingFactor);
        return true;
    }

    TxKernel::Ptr BaseTransaction::createKernel(Amount fee, Height minHeight) const
    {
        auto kernel = make_unique<TxKernel>();
        kernel->m_Fee = fee;
        kernel->m_Height.m_Min = minHeight;
        kernel->m_Height.m_Max = MaxHeight;
        kernel->m_Excess = Zero;
        return kernel;
    }

    Signature::MultiSig BaseTransaction::createMultiSig(const TxKernel& kernel, const Scalar::Native& blindingExcess) const
    {
        Signature::MultiSig msig;
        Hash::Value hv;
        kernel.get_Hash(hv);

        msig.GenerateNonce(hv, blindingExcess);
        return msig;
    }

    //Scalar::Native BaseTransaction::createSignature(const TxKernel& kernel, const Scalar::Native& blindingExcess, const Signature::MultiSig& msig) const
    //{
    //    Hash::Value message;
    //    kernel.get_Hash(message);
    //    Scalar::Native partialSignature;
    ////    kernel.m_Signature.CoSign(partialSignature, message, blindingExcess, msig);
    //    return partialSignature;
    //}

    void BaseTransaction::createOutputUtxo(Amount amount, Height height)
    {
        Coin newUtxo{ amount, Coin::Draft, height };
        newUtxo.m_createTxId = m_txDesc.m_txId;
        m_keychain->store(newUtxo);

        Scalar::Native blindingFactor = m_keychain->calcKey(newUtxo);
        auto[privateExcess, offset] = splitKey(blindingFactor, newUtxo.m_id);

        blindingFactor = -privateExcess;
        m_blindingExcess += blindingFactor;
        m_offset += offset;
    }

    Scalar BaseTransaction::createSignature()
    {
        Point publicNonce;
        Scalar partialSignature;
        createSignature2(partialSignature, publicNonce, m_kernel->m_Signature.m_e);
        return partialSignature;
    }

    void BaseTransaction::get_NonceInternal(ECC::Signature::MultiSig& out) const
    {
        Point pt = m_kernel->m_Excess;
        m_kernel->m_Excess = Zero;

        Hash::Value hv;
        m_kernel->get_Hash(hv);

        m_kernel->m_Excess = pt;

        out.GenerateNonce(hv, m_blindingExcess);
    }

    void BaseTransaction::onFailed(bool notify)
    {
        updateTxDescription(TxDescription::Failed);
        rollbackTx();
        if (notify)
        {
            m_gateway.send_tx_failed(m_txDesc);
        }
        m_gateway.on_tx_completed(m_txDesc);
    }

    void BaseTransaction::createSignature2(Scalar& signature, Point& publicNonce, Scalar& challenge) const
    {
        Signature::MultiSig msig;
        get_NonceInternal(msig);

        Point::Native pt = Context::get().G * msig.m_Nonce;
        publicNonce = pt;
        msig.m_NoncePub = m_publicPeerNonce + pt;

        pt = Context::get().G * m_blindingExcess;
        pt += m_publicPeerExcess;
        m_kernel->m_Excess = pt;
        Hash::Value message;
        m_kernel->get_Hash(message);

        Scalar::Native partialSignature;
        Signature sig;
        sig.CoSign(partialSignature, message, m_blindingExcess, msig);
        challenge = sig.m_e;
        signature = partialSignature;
    }

    Point BaseTransaction::getPublicExcess() const
    {
        return Point(Context::get().G * m_blindingExcess);
    }

    Point BaseTransaction::getPublicNonce() const
    {
        Signature::MultiSig msig;
        get_NonceInternal(msig);

        return Point(Context::get().G * msig.m_Nonce);
    }

    bool BaseTransaction::isValidSignature(const Scalar::Native& peerSignature) const
    {
        return isValidSignature(peerSignature, m_publicPeerNonce, m_publicPeerExcess);
    }

    bool BaseTransaction::isValidSignature(const Scalar::Native& peerSignature, const Point::Native& publicPeerNonce, const Point::Native& publicPeerExcess) const
    {
        //assert(m_kernel);
        if (!m_kernel)
            return false;

        Signature::MultiSig msig;
        get_NonceInternal(msig);

        Point::Native publicNonce = Context::get().G * msig.m_Nonce;

        msig.m_NoncePub = publicNonce + publicPeerNonce;

        Point::Native pt = Context::get().G * m_blindingExcess;
        pt += publicPeerExcess;
        m_kernel->m_Excess = pt;

        Hash::Value message;
        m_kernel->get_Hash(message);

        // temp signature to calc challenge
        Scalar::Native mySig;
        Signature peerSig;
        peerSig.CoSign(mySig, message, m_blindingExcess, msig);
        peerSig.m_k = peerSignature;
        return peerSig.IsValidPartial(publicPeerNonce, publicPeerExcess);
    }

    vector<Input::Ptr> BaseTransaction::getTxInputs(const TxID& txID) const
    {
        vector<Input::Ptr> inputs;
        m_keychain->visit([this, &txID, &inputs](const Coin& c)->bool
        {
            if (c.m_spentTxId == txID && c.m_status == Coin::Locked)
            {
                Input::Ptr input = make_unique<Input>();

                Scalar::Native blindingFactor = m_keychain->calcKey(c);
                input->m_Commitment = Commitment(blindingFactor, c.m_amount);

                inputs.push_back(move(input));
            }
            return true;
        });
        return inputs;
    }

    vector<Output::Ptr> BaseTransaction::getTxOutputs(const TxID& txID) const
    {
        vector<Output::Ptr> outputs;
        m_keychain->visit([this, &txID, &outputs](const Coin& c)->bool
        {
            if (c.m_createTxId == txID && c.m_status == Coin::Draft)
            {
                Output::Ptr output = make_unique<Output>();
                output->m_Coinbase = false;

                Scalar::Native blindingFactor = m_keychain->calcKey(c);
                output->Create(blindingFactor, c.m_amount);

                outputs.push_back(move(output));
            }
            return true;
        });
        return outputs;
    }
}}
