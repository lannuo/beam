#include "address_book.h"
#include "ui_helpers.h"

using namespace std;
using namespace beam;
using namespace beamui;

PeerAddressItem::PeerAddressItem(const beam::WalletAddress& address)
    : m_walletID{beamui::toString(address.m_walletID)}
    , m_name{QString::fromStdString(address.m_label)}
{

}

QString PeerAddressItem::getWalletID() const
{
    return m_walletID;
}

QString PeerAddressItem::getName() const
{
    return m_name;
}

void PeerAddressItem::setName(const QString& value)
{
    m_name = value;
}

QString PeerAddressItem::getCategory() const
{
    return m_category;
}

void PeerAddressItem::setCategory(const QString& value)
{
    m_category = value;
}

OwnAddressItem::OwnAddressItem(const beam::WalletAddress& address) 
    : PeerAddressItem{ address }
    , m_createDate{toString(address.m_createTime)}
    , m_expirationDate{toString(address.m_createTime + address.m_duration)}
{

}

QString OwnAddressItem::getExpirationDate() const
{
    return m_expirationDate;
}

QString OwnAddressItem::getCreateDate() const
{
    return m_createDate;
}


AddressBookViewModel::AddressBookViewModel(WalletModel& model)
    : m_model{model}
{
    connect(&m_model, SIGNAL(OnAdrresses(bool own, const std::vector<beam::WalletAddress>&)),
        SLOT(oOnAdrresses(bool own, const std::vector<beam::WalletAddress>&)));

    m_model.async->getAddresses(true);
    m_model.async->getAddresses(false);
}

QVariant AddressBookViewModel::getPeerAddresses() const
{
    return QVariant::fromValue(m_peerAddresses);
}

QVariant AddressBookViewModel::getOwnAddresses() const
{
    return QVariant::fromValue(m_ownAddresses);
}

void AddressBookViewModel::OnAdrresses(bool own, const std::vector<beam::WalletAddress>& addresses)
{
    if (own)
    {
        for (const auto& addr : addresses)
        {
            m_ownAddresses.push_back(new OwnAddressItem(addr));
        }
    }
    else
    {
        for (const auto& addr : addresses)
        {
            m_peerAddresses.push_back(new PeerAddressItem(addr));
        }
    }

    emit addressesChanged();
}