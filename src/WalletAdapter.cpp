// Copyright (c) 2011-2016 The Cryptonote developers
// Copyright (c) 2015-2016 XDN developers
// Copyright (c) 2016-2017 The Chavezcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <QCoreApplication>
#include <QDateTime>
#include <QLocale>
#include <QVector>
#include <QDebug>

#include <Common/Base58.h>
#include <Common/Util.h>
#include <Wallet/WalletErrors.h>
#include <Wallet/LegacyKeysImporter.h>

#include "NodeAdapter.h"
#include "Settings.h"
#include "WalletAdapter.h"

namespace WalletGui {

const quint32 MSECS_IN_HOUR = 60 * 60 * 1000;
const quint32 MSECS_IN_MINUTE = 60 * 1000;

const quint32 LAST_BLOCK_INFO_UPDATING_INTERVAL = 1 * MSECS_IN_MINUTE;
const quint32 LAST_BLOCK_INFO_WARNING_INTERVAL = 1 * MSECS_IN_HOUR;

WalletAdapter& WalletAdapter::instance() {
  static WalletAdapter inst;
  return inst;
}

WalletAdapter::WalletAdapter() : QObject(), m_wallet(nullptr), m_mutex(), m_isBackupInProgress(false),
  m_isSynchronized(false), m_newTransactionsNotificationTimer(),
  m_lastWalletTransactionId(std::numeric_limits<quint64>::max()) {
  connect(this, &WalletAdapter::walletInitCompletedSignal, this, &WalletAdapter::onWalletInitCompleted, Qt::QueuedConnection);
  connect(this, &WalletAdapter::walletSendTransactionCompletedSignal, this, &WalletAdapter::onWalletSendTransactionCompleted, Qt::QueuedConnection);
  connect(this, &WalletAdapter::updateBlockStatusTextSignal, this, &WalletAdapter::updateBlockStatusText, Qt::QueuedConnection);
  connect(this, &WalletAdapter::updateBlockStatusTextWithDelaySignal, this, &WalletAdapter::updateBlockStatusTextWithDelay, Qt::QueuedConnection);
  connect(&m_newTransactionsNotificationTimer, &QTimer::timeout, this, &WalletAdapter::notifyAboutLastTransaction);
  connect(this, &WalletAdapter::walletSynchronizationProgressUpdatedSignal, this, [&]() {
    if (!m_newTransactionsNotificationTimer.isActive()) {
      m_newTransactionsNotificationTimer.start();
    }
  }, Qt::QueuedConnection);

  connect(this, &WalletAdapter::walletSynchronizationCompletedSignal, this, [&]() {
    m_newTransactionsNotificationTimer.stop();
    notifyAboutLastTransaction();
  }, Qt::QueuedConnection);

  m_newTransactionsNotificationTimer.setInterval(500);
}

WalletAdapter::~WalletAdapter() {
}

QString WalletAdapter::getAddress() const {
  try {
    return m_wallet == nullptr ? QString() : QString::fromStdString(m_wallet->getAddress());
  } catch (std::system_error&) {
    return QString();
  }
}

quint64 WalletAdapter::getActualBalance() const {
  try {
    return m_wallet == nullptr ? 0 : m_wallet->actualBalance();
  } catch (std::system_error&) {
    return 0;
  }
}

quint64 WalletAdapter::getPendingBalance() const {
  try {
    return m_wallet == nullptr ? 0 : m_wallet->pendingBalance();
  } catch (std::system_error&) {
    return 0;
  }
}

void WalletAdapter::open(const QString& _password) {
  Q_ASSERT(m_wallet == nullptr);
  Settings::instance().setEncrypted(!_password.isEmpty());
  Q_EMIT walletStateChangedSignal(tr("Opening wallet"));

  m_wallet = NodeAdapter::instance().createWallet();
  m_wallet->addObserver(this);

  if (QFile::exists(Settings::instance().getWalletFile())) {

    backupOnOpen(); // Backup wallet

    if (Settings::instance().getWalletFile().endsWith(".keys")) {
      if (!importLegacyWallet(_password)) {
        return;
      }
    }

    if (openFile(Settings::instance().getWalletFile(), true)) {
      try {
        m_wallet->initAndLoad(m_file, _password.toStdString());
      } catch (std::system_error&) {
        closeFile();
        delete m_wallet;
        m_wallet = nullptr;
      }
    }
  } else {
    Settings::instance().setEncrypted(false);
    try {
      m_wallet->initAndGenerate("");
    } catch (std::system_error&) {
      delete m_wallet;
      m_wallet = nullptr;
    }
  }
}

void WalletAdapter::createWithKeys(const CryptoNote::AccountKeys& _keys) {
    m_wallet = NodeAdapter::instance().createWallet();
    m_wallet->addObserver(this);
    Settings::instance().setEncrypted(false);
    Q_EMIT walletStateChangedSignal(tr("Importing keys"));
    m_wallet->initWithKeys(_keys, "");
}


bool WalletAdapter::isOpen() const {
  return m_wallet != nullptr;
}

bool WalletAdapter::importLegacyWallet(const QString &_password) {
  QString fileName = Settings::instance().getWalletFile();
  Settings::instance().setEncrypted(!_password.isEmpty());
  try {
    fileName.replace(fileName.lastIndexOf(".keys"), 5, ".wallet");
    if (!openFile(fileName, false)) {
      delete m_wallet;
      m_wallet = nullptr;
      return false;
    }

    CryptoNote::importLegacyKeys(Settings::instance().getWalletFile().toStdString(), _password.toStdString(), m_file);
    closeFile();
    Settings::instance().setWalletFile(fileName);
    return true;
  } catch (std::system_error& _err) {
    closeFile();
    if (_err.code().value() == CryptoNote::error::WRONG_PASSWORD) {
      Settings::instance().setEncrypted(true);
      Q_EMIT openWalletWithPasswordSignal(!_password.isEmpty());
    }
  } catch (std::runtime_error& _err) {
    closeFile();
  }

  delete m_wallet;
  m_wallet = nullptr;
  return false;
}

void WalletAdapter::close() {
  Q_CHECK_PTR(m_wallet);
  save(true, true);
  lock();
  m_wallet->removeObserver(this);
  m_isSynchronized = false;
  m_newTransactionsNotificationTimer.stop();
  m_lastWalletTransactionId = std::numeric_limits<quint64>::max();
  Q_EMIT walletCloseCompletedSignal();
  QCoreApplication::processEvents();
  delete m_wallet;
  m_wallet = nullptr;
  unlock();
}

bool WalletAdapter::save(bool _details, bool _cache) {
  return save(Settings::instance().getWalletFile() + ".temp", _details, _cache);
}

bool WalletAdapter::save(const QString& _file, bool _details, bool _cache) {
  Q_CHECK_PTR(m_wallet);
  if (openFile(_file, false)) {
    try {
      m_wallet->save(m_file, _details, _cache);
    } catch (std::system_error&) {
      closeFile();
      return false;
    }
    Q_EMIT walletStateChangedSignal(tr("Saving data"));
  } else {
    return false;
  }

  return true;
}

void WalletAdapter::backup(const QString& _file) {
  if (save(_file.endsWith(".wallet") ? _file : _file + ".wallet", true, false)) {
    m_isBackupInProgress = true;
  }
}

void WalletAdapter::backupOnOpen(){
  QFileInfo sourceInfo(Settings::instance().getWalletFile()), destinationInfo(Settings::instance().getDataDir().absoluteFilePath("backup"));
  if (!QDir(Settings::instance().getDataDir().absoluteFilePath("backup")).exists())
       QDir().mkdir(Settings::instance().getDataDir().absoluteFilePath("backup"));

  QString source = Settings::instance().getWalletFile();
  QString destination = (QFileInfo(QDir(destinationInfo.absoluteFilePath()), sourceInfo.fileName())).filePath();
  QString dateTimeStr = QLocale(QLocale::English).toString(QDateTime::currentDateTime(), ".dd-MM-yyyy-HH-mm");
  destination.replace(destination.lastIndexOf(".wallet"), 7, dateTimeStr + ".wallet");

  if (QFile::exists(destination)) {
      QFile::remove(destination);
  }
  QFile::copy(source, destination);
  qDebug() << qPrintable(QString("Creating backup of %1 -> %2\n").arg(source).arg(destination));

  int counter = 0;
  QString walletFileName = sourceInfo.fileName();
  QString walletName = walletFileName.section(".",0,0);
  QDir dir = Settings::instance().getDataDir().absoluteFilePath("backup");
  QStringList filters;
       filters << walletName + "*.wallet";
       dir.setNameFilters(filters);
       dir.setFilter(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
       dir.setSorting(QDir::Time);
  QFileInfoList list = dir.entryInfoList();

  for (int i = 0; i < list.size(); ++i) {
    QFileInfo fileInfo = list.at(i);
    counter++;
    if (counter > 10){ // More than ten backups: delete oldest one(s)
      QFile::remove(fileInfo.absoluteFilePath());
      qDebug() << qPrintable(QString("Old backup deleted: %1").arg(fileInfo.fileName()));
    }
  }
}

void WalletAdapter::reset() {
  Q_CHECK_PTR(m_wallet);
  save(false, false);
  lock();
  m_wallet->removeObserver(this);
  m_isSynchronized = false;
  m_newTransactionsNotificationTimer.stop();
  m_lastWalletTransactionId = std::numeric_limits<quint64>::max();
  Q_EMIT walletCloseCompletedSignal();
  QCoreApplication::processEvents();
  delete m_wallet;
  m_wallet = nullptr;
  unlock();
}

quint64 WalletAdapter::getTransactionCount() const {
  Q_CHECK_PTR(m_wallet);
  try {
    return m_wallet->getTransactionCount();
  } catch (std::system_error&) {
  }

  return 0;
}

quint64 WalletAdapter::getTransferCount() const {
  Q_CHECK_PTR(m_wallet);
  try {
    return m_wallet->getTransferCount();
  } catch (std::system_error&) {
  }

  return 0;
}

bool WalletAdapter::getTransaction(CryptoNote::TransactionId& _id, CryptoNote::WalletLegacyTransaction& _transaction) {
  Q_CHECK_PTR(m_wallet);
  try {
    return m_wallet->getTransaction(_id, _transaction);
  } catch (std::system_error&) {
  }

  return false;
}

bool WalletAdapter::getTransfer(CryptoNote::TransferId& _id, CryptoNote::WalletLegacyTransfer& _transfer) {
  Q_CHECK_PTR(m_wallet);
  try {
    return m_wallet->getTransfer(_id, _transfer);
  } catch (std::system_error&) {
  }

  return false;
}

bool WalletAdapter::getAccountKeys(CryptoNote::AccountKeys& _keys) {
  Q_CHECK_PTR(m_wallet);
  try {
    m_wallet->getAccountKeys(_keys);
    return true;
  } catch (std::system_error&) {
  }

  return false;
}

void WalletAdapter::sendTransaction(const QVector<CryptoNote::WalletLegacyTransfer>& _transfers, quint64 _fee, const QString& _paymentId, quint64 _mixin) {
  Q_CHECK_PTR(m_wallet);
  try {
    lock();
    m_wallet->sendTransaction(_transfers.toStdVector(), _fee, NodeAdapter::instance().convertPaymentId(_paymentId), _mixin, 0);
    Q_EMIT walletStateChangedSignal(tr("Sending transaction"));
  } catch (std::system_error&) {
    unlock();
  }
}

bool WalletAdapter::changePassword(const QString& _oldPassword, const QString& _newPassword) {
  Q_CHECK_PTR(m_wallet);
  try {
    if (m_wallet->changePassword(_oldPassword.toStdString(), _newPassword.toStdString()).value() == CryptoNote::error::WRONG_PASSWORD) {
      return false;
    }
  } catch (std::system_error&) {
    return false;
  }

  Settings::instance().setEncrypted(!_newPassword.isEmpty());
  return save(true, true);
}

void WalletAdapter::setWalletFile(const QString& _path) {
  Q_ASSERT(m_wallet == nullptr);
  Settings::instance().setWalletFile(_path);
}

void WalletAdapter::initCompleted(std::error_code _error) {
  if (m_file.is_open()) {
    closeFile();
  }

  Q_EMIT walletInitCompletedSignal(_error.value(), QString::fromStdString(_error.message()));
}

void WalletAdapter::onWalletInitCompleted(int _error, const QString& _errorText) {
  switch(_error) {
  case 0: {
    Q_EMIT walletActualBalanceUpdatedSignal(m_wallet->actualBalance());
    Q_EMIT walletPendingBalanceUpdatedSignal(m_wallet->pendingBalance());
    Q_EMIT updateWalletAddressSignal(QString::fromStdString(m_wallet->getAddress()));
    Q_EMIT reloadWalletTransactionsSignal();
    Q_EMIT walletStateChangedSignal(tr("Ready"));
    QTimer::singleShot(5000, this, SLOT(updateBlockStatusText()));
    if (!QFile::exists(Settings::instance().getWalletFile())) {
      save(true, true);
    }

    break;
  }
  case CryptoNote::error::WRONG_PASSWORD:
    Q_EMIT openWalletWithPasswordSignal(Settings::instance().isEncrypted());
    Settings::instance().setEncrypted(true);
    delete m_wallet;
    m_wallet = nullptr;
    break;
  default: {
    delete m_wallet;
    m_wallet = nullptr;
    break;
  }
  }
}

void WalletAdapter::saveCompleted(std::error_code _error) {
  if (!_error && !m_isBackupInProgress) {
    closeFile();
    renameFile(Settings::instance().getWalletFile() + ".temp", Settings::instance().getWalletFile());
    Q_EMIT walletStateChangedSignal(tr("Ready"));
    Q_EMIT updateBlockStatusTextWithDelaySignal();
  } else if (m_isBackupInProgress) {
    m_isBackupInProgress = false;
    closeFile();
  } else {
    closeFile();
  }

  Q_EMIT walletSaveCompletedSignal(_error.value(), QString::fromStdString(_error.message()));
}

void WalletAdapter::synchronizationProgressUpdated(uint32_t _current, uint32_t _total) {
  m_isSynchronized = false;
  Q_EMIT walletStateChangedSignal(QString("%1 %2/%3").arg(tr("Synchronizing")).arg(_current).arg(_total));
  Q_EMIT walletSynchronizationProgressUpdatedSignal(_current, _total);
}

void WalletAdapter::synchronizationCompleted(std::error_code _error) {
  if (!_error) {
    m_isSynchronized = true;
    Q_EMIT updateBlockStatusTextSignal();
    Q_EMIT walletSynchronizationCompletedSignal(_error.value(), QString::fromStdString(_error.message()));
  }
}

void WalletAdapter::actualBalanceUpdated(uint64_t _actual_balance) {
  Q_EMIT walletActualBalanceUpdatedSignal(_actual_balance);
}

void WalletAdapter::pendingBalanceUpdated(uint64_t _pending_balance) {
  Q_EMIT walletPendingBalanceUpdatedSignal(_pending_balance);
}

void WalletAdapter::externalTransactionCreated(CryptoNote::TransactionId _transactionId) {
  if (!m_isSynchronized) {
    m_lastWalletTransactionId = _transactionId;
  } else {
    Q_EMIT walletTransactionCreatedSignal(_transactionId);
  }
}

void WalletAdapter::sendTransactionCompleted(CryptoNote::TransactionId _transaction_id, std::error_code _error) {
  unlock();
  Q_EMIT walletSendTransactionCompletedSignal(_transaction_id, _error.value(), walletErrorMessage(_error.value()));
  Q_EMIT updateBlockStatusTextWithDelaySignal();
}

QString WalletAdapter::walletErrorMessage(int _error_code) {
  switch (_error_code) {
    case CryptoNote::error::WalletErrorCodes::NOT_INITIALIZED:               return tr("Object was not initialized");
    case CryptoNote::error::WalletErrorCodes::WRONG_PASSWORD:                return tr("The password is wrong");
    case CryptoNote::error::WalletErrorCodes::ALREADY_INITIALIZED:           return tr("The object is already initialized");
    case CryptoNote::error::WalletErrorCodes::INTERNAL_WALLET_ERROR:         return tr("Internal error occurred");
    case CryptoNote::error::WalletErrorCodes::MIXIN_COUNT_TOO_BIG:           return tr("MixIn count is too big");
    case CryptoNote::error::WalletErrorCodes::BAD_ADDRESS:                   return tr("Bad address");
    case CryptoNote::error::WalletErrorCodes::TRANSACTION_SIZE_TOO_BIG:      return tr("Transaction size is too big");
    case CryptoNote::error::WalletErrorCodes::WRONG_AMOUNT:                  return tr("Wrong amount");
    case CryptoNote::error::WalletErrorCodes::SUM_OVERFLOW:                  return tr("Sum overflow");
    case CryptoNote::error::WalletErrorCodes::ZERO_DESTINATION:              return tr("The destination is empty");
    case CryptoNote::error::WalletErrorCodes::TX_CANCEL_IMPOSSIBLE:          return tr("Impossible to cancel transaction");
    case CryptoNote::error::WalletErrorCodes::WRONG_STATE:                   return tr("The wallet is in wrong state (maybe loading or saving), try again later");
    case CryptoNote::error::WalletErrorCodes::OPERATION_CANCELLED:           return tr("The operation you've requested has been cancelled");
    case CryptoNote::error::WalletErrorCodes::TX_TRANSFER_IMPOSSIBLE:        return tr("Transaction transfer impossible");
    case CryptoNote::error::WalletErrorCodes::WRONG_VERSION:                 return tr("Wrong version");
    case CryptoNote::error::WalletErrorCodes::FEE_TOO_SMALL:                 return tr("Transaction fee is too small");
    case CryptoNote::error::WalletErrorCodes::KEY_GENERATION_ERROR:          return tr("Cannot generate new key");
    case CryptoNote::error::WalletErrorCodes::INDEX_OUT_OF_RANGE:            return tr("Index is out of range");
    case CryptoNote::error::WalletErrorCodes::ADDRESS_ALREADY_EXISTS:        return tr("Address already exists");
    case CryptoNote::error::WalletErrorCodes::TRACKING_MODE:                 return tr("The wallet is in tracking mode");
    case CryptoNote::error::WalletErrorCodes::WRONG_PARAMETERS:              return tr("Wrong parameters passed");
    case CryptoNote::error::WalletErrorCodes::OBJECT_NOT_FOUND:              return tr("Object not found");
    case CryptoNote::error::WalletErrorCodes::WALLET_NOT_FOUND:              return tr("Requested wallet not found");
    case CryptoNote::error::WalletErrorCodes::CHANGE_ADDRESS_REQUIRED:       return tr("Change address required");
    case CryptoNote::error::WalletErrorCodes::CHANGE_ADDRESS_NOT_FOUND:      return tr("Change address not found");
    case CryptoNote::error::WalletErrorCodes::DESTINATION_ADDRESS_REQUIRED:  return tr("Destination address required");
    case CryptoNote::error::WalletErrorCodes::DESTINATION_ADDRESS_NOT_FOUND: return tr("Destination address not found");
    case CryptoNote::error::WalletErrorCodes::BAD_PAYMENT_ID:                return tr("Wrong payment id format");
    case CryptoNote::error::WalletErrorCodes::BAD_TRANSACTION_EXTRA:         return tr("Wrong transaction extra format");
    default:                                                                 return tr("Unknown error");
  }
}

void WalletAdapter::onWalletSendTransactionCompleted(CryptoNote::TransactionId _transactionId, int _error, const QString& _errorText) {
  if (_error) {
    return;
  }

  CryptoNote::WalletLegacyTransaction transaction;
  if (!this->getTransaction(_transactionId, transaction)) {
    return;
  }

  if (transaction.transferCount == 0) {
    return;
  }

  Q_EMIT walletTransactionCreatedSignal(_transactionId);

  save(true, true);
}

void WalletAdapter::transactionUpdated(CryptoNote::TransactionId _transactionId) {
  Q_EMIT walletTransactionUpdatedSignal(_transactionId);
}

void WalletAdapter::lock() {
  m_mutex.lock();
}

void WalletAdapter::unlock() {
  m_mutex.unlock();
}

bool WalletAdapter::openFile(const QString& _file, bool _readOnly) {
  lock();


#ifdef Q_OS_WIN
  m_file.open(_file.toStdWString(), std::ios::binary | (_readOnly ? std::ios::in : (std::ios::out | std::ios::trunc)));
#else
  m_file.open(_file.toStdString(), std::ios::binary | (_readOnly ? std::ios::in : (std::ios::out | std::ios::trunc)));
#endif


  if (!m_file.is_open()) {
    unlock();
  }

  return m_file.is_open();
}

void WalletAdapter::closeFile() {
  m_file.close();
  unlock();
}

void WalletAdapter::notifyAboutLastTransaction() {
  if (m_lastWalletTransactionId != std::numeric_limits<quint64>::max()) {
    Q_EMIT walletTransactionCreatedSignal(m_lastWalletTransactionId);
    m_lastWalletTransactionId = std::numeric_limits<quint64>::max();
  }
}

void WalletAdapter::renameFile(const QString& _oldName, const QString& _newName) {
  Q_ASSERT(QFile::exists(_oldName));
  QFile::remove(_newName);
  QFile::rename(_oldName, _newName);
}

void WalletAdapter::updateBlockStatusText() {
  if (m_wallet == nullptr) {
    return;
  }

  const QDateTime currentTime = QDateTime::currentDateTimeUtc();
  const QDateTime blockTime = NodeAdapter::instance().getLastLocalBlockTimestamp();
  quint64 blockAge = blockTime.msecsTo(currentTime);
  const QString warningString = blockTime.msecsTo(currentTime) < LAST_BLOCK_INFO_WARNING_INTERVAL ? "" :
    QString(tr("  Warning: last block was received %1 hours %2 minutes ago")).arg(blockAge / MSECS_IN_HOUR).arg(blockAge % MSECS_IN_HOUR / MSECS_IN_MINUTE);
  Q_EMIT walletStateChangedSignal(QString(tr("Wallet synchronized. Height: %1  |  Time (UTC): %2%3")).
    arg(NodeAdapter::instance().getLastLocalBlockHeight()).
    arg(QLocale(QLocale::English).toString(blockTime, "dd.MM.yyyy, HH:mm:ss")).
    arg(warningString));

  QTimer::singleShot(LAST_BLOCK_INFO_UPDATING_INTERVAL, this, SLOT(updateBlockStatusText()));
}

void WalletAdapter::updateBlockStatusTextWithDelay() {
  QTimer::singleShot(5000, this, SLOT(updateBlockStatusText()));
}

}
