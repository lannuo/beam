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

#pragma once

#include "wallet/common.h"
#include "wallet/wallet_db.h"

#include <boost/optional.hpp>
#include "utility/logger.h"

namespace beam { namespace wallet
{
    struct ITransaction
    {
        using Ptr = std::shared_ptr<ITransaction>;
        virtual void update() = 0;
        virtual void cancel() = 0;
    };

    enum class TxParams : uint32_t 
    {
        Amount,
        Fee,
        MinHeight,
        Offset,
        Inputs,
        Outputs,
        BlindingExcess,
        PeerSignature,
        PublicPeerNonce,
        PublicPeerExcess,
        PeerOffset,
        PeerInputs,
        PeerOutputs,
        TransactionRegistered,
        KernelProof,
        FailureReason
    };

    //
    // State machine for managing per transaction negotiations between wallets
    // 
    class BaseTransaction : public ITransaction
    {
    public:
        using Ptr = std::shared_ptr<BaseTransaction>;
        BaseTransaction(INegotiatorGateway& gateway
            , beam::IKeyChain::Ptr keychain
            , const TxDescription& txDesc);

        //void saveState();

        TxKernel* getKernel() const;

        const TxID& getTxID() const;
        void cancel() override;
    protected:
 
        void sendNewTransaction() const;
        void confirmKernel(const TxKernel& kernel);
        void confirmOutputs();
        void completeTx();
        void rollbackTx();
        void updateTxDescription(TxDescription::Status s);
        bool prepareSenderUtxos(const Height& currentHeight);
        bool registerTxInternal(const ECC::Scalar& peerSignature);
        bool constructTxInternal(const ECC::Scalar::Native& signature);
        TxKernel::Ptr createKernel(Amount fee, Height minHeight) const;
        ECC::Signature::MultiSig createMultiSig(const TxKernel& kernel, const ECC::Scalar::Native& blindingExcess) const;
        //ECC::Scalar::Native createSignature(const TxKernel& kernel, const ECC::Scalar::Native& blindingExcess, ECC::Signature::MultiSig& msig) const;
        void createOutputUtxo(Amount amount, Height height);
        ECC::Scalar createSignature();
        void createSignature2(ECC::Scalar& partialSignature, ECC::Point& publicNonce, ECC::Scalar& challenge) const;
        ECC::Point getPublicExcess() const;
        ECC::Point getPublicNonce() const;
        bool isValidSignature(const ECC::Scalar::Native& peerSignature) const;
        bool isValidSignature(const ECC::Scalar::Native& peerSignature, const ECC::Point::Native& publicPeerNonce, const ECC::Point::Native& publicPeerExcess) const;
        std::vector<Input::Ptr> getTxInputs(const TxID& txID) const;
        std::vector<Output::Ptr> getTxOutputs(const TxID& txID) const;
		void get_NonceInternal(ECC::Signature::MultiSig&) const;

        template <typename T>
        bool getParameter(TxParams paramID, T& value)
        {
            ByteBuffer b;
            if (m_keychain->getTxParameter(m_txDesc.m_txId, static_cast<uint32_t>(paramID), b))
            {
                Deserializer d;
                d.reset(b.data(), b.size());
                d & value;
                return true;
            }
            return false;
        }

        template <typename T>
        void setParameter(TxParams paramID, const T& value)
        {
            Serializer s;
            s & value;
            ByteBuffer b;
            s.swap_buf(b);
            m_keychain->setTxParameter(m_txDesc.m_txId, static_cast<uint32_t>(paramID), std::move(b));
        }


        bool getParameter(TxParams paramID, ECC::Point::Native& value);
        bool getParameter(TxParams paramID, ECC::Scalar::Native& value);

        void setParameter(TxParams paramID, const ECC::Point::Native& value);
        void setParameter(TxParams paramID, const ECC::Scalar::Native& value);

        void onFailed(bool notify = false);

        bool getTip(Block::SystemState::Full& state) const;

    protected:

        ECC::Scalar::Native m_blindingExcess;
        ECC::Scalar::Native m_offset;
        ECC::Scalar::Native m_peerSignature;
        ECC::Point::Native m_publicPeerExcess;
        ECC::Point::Native m_publicPeerNonce;
        Transaction::Ptr m_transaction;
        TxKernel::Ptr m_kernel;

        INegotiatorGateway& m_gateway;
        beam::IKeyChain::Ptr m_keychain;

        TxDescription m_txDesc;
    };

    class SendTransaction : public BaseTransaction
    {
    public:
        SendTransaction(INegotiatorGateway& gateway
            , beam::IKeyChain::Ptr keychain
            , const TxDescription& txDesc);
        void update() override;
    private:
        void sendSelfTx();
        void sendInvite() const;
        void sendConfirmTransaction(const ECC::Scalar& peerSignature) const;
    };

    class ReceiveTransaction : public BaseTransaction
    {
    public:
        ReceiveTransaction(INegotiatorGateway& gateway
            , beam::IKeyChain::Ptr keychain
            , const TxDescription& txDesc);
        void update() override;
    private:
        void sendConfirmInvitation() const;
    };
}}
