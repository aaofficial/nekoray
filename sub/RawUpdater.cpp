#include <QInputDialog>
#include "RawUpdater.hpp"

#ifndef __MINGW32__

#include <yaml-cpp/yaml.h>

#endif

#include "db/Database.hpp"
#include "ui/mainwindow_message.h"
#include "qv2ray/utils/HTTPRequestHelper.hpp"
#include "db/filter/ProfileFilter.hpp"

namespace NekoRay::sub {
    void RawUpdater::update(const QString &str) {
        // Base64 encoded subscription
        if (auto str2 = DecodeB64IfValid(str);!str2.isEmpty()) {
            update(str2);
            return;
        }

        // Clash
        if (str.contains("proxies:")) {
            updateClash(str);
            return;
        }

        // Multi line
        if (str.count("\n") > 0) {
            auto list = str.split("\n");
            for (const auto &str2: list) {
                update(str2);
            }
            return;
        }

        QSharedPointer<ProxyEntity> ent;

        // SOCKS
        if (str.startsWith("socks5://") || str.startsWith("socks4://")) {
            ent = ProfileManager::NewProxyEntity("socks");
            auto ok = ent->SocksBean()->ParseStdLink(str);
            if (!ok) ent = nullptr;
        }

        // ShadowSocks
        if (str.startsWith("ss://")) {
            ent = ProfileManager::NewProxyEntity("shadowsocks");
            auto ok = ent->ShadowSocksBean()->ParseStdLink(str);
            if (!ok) ent = nullptr;
        }

        // VMess
        if (str.startsWith("vmess://")) {
            ent = ProfileManager::NewProxyEntity("vmess");
            auto ok = ent->VMessBean()->TryParseLink(str);
            if (!ok) ent = nullptr;
        }

        // End
        if (ent.get() == nullptr) return;
        profileManager->AddProfile(ent, update_sub_gid);
        dataStore->updated_count++;
    }

#ifndef __MINGW32__

    QString Node2QString(const YAML::Node &n) {
        return {n.Scalar().c_str()};
    }

#endif

    void RawUpdater::updateClash(const QString &str) {
#ifndef __MINGW32__
        try {
            auto proxies = YAML::Load(str.toStdString())["proxies"];
            for (auto proxy: proxies) {
                auto type = Node2QString(proxy["type"]);
                if (type == "ss") type = "shadowsocks";

                auto ent = ProfileManager::NewProxyEntity(type);
                ent->bean->name = Node2QString(proxy["name"]);
                ent->bean->serverAddress = Node2QString(proxy["server"]);
                ent->bean->serverPort = proxy["port"].as<int>();

                if (type == "shadowsocks") {
                    auto bean = ent->ShadowSocksBean();
                    bean->method = Node2QString(proxy["cipher"]).replace("dummy", "none");
                    bean->password = Node2QString(proxy["password"]);
                    auto plugin_n = proxy["plugin"];
                    auto pluginOpts_n = proxy["plugin-opts"];
                    if (plugin_n.IsDefined() && pluginOpts_n.IsDefined()) {
                        if (Node2QString(plugin_n) == "obfs") {
                            bean->plugin = "obfs-local;obfs=" + Node2QString(pluginOpts_n["mode"]) + ";obfs-host=" +
                                           Node2QString(pluginOpts_n["host"]);
                        }
                    }
                } else {
                    // TODO impl
                    continue;
                }

                profileManager->AddProfile(ent, update_sub_gid);
                dataStore->updated_count++;
            }
        } catch (const YAML::Exception &ex) {
            MessageBoxWarning("YAML Exception", ex.what());
        }
#endif
    }

    // 各种奇妙UI
    // 不要刷新，下载导入完会自己刷新
    void RawUpdater::AsyncUpdate(const QString &str, int _update_sub_gid) {
        dataStore->updated_count = 0;
        this->update_sub_gid = _update_sub_gid;

        auto content = str.trimmed();
        bool asURL = update_sub_gid >= 0; // 订阅

        if (!asURL && (content.startsWith("http://") || content.startsWith("https://"))) {
            auto items = QStringList{"作为订阅链接", "作为节点"};
            bool ok;
            auto a = QInputDialog::getItem(nullptr,
                                           "url detected",
                                           content + "\nhow to update?",
                                           items, 0, false, &ok);
            if (!ok) return;
            if (items.indexOf(a) == 0) asURL = true;
        }

        runOnNewThread([=] {
            auto content2 = content;
            auto group = profileManager->GetGroup(update_sub_gid);

            if (asURL) {
                showLog("URL=" + content2);
                content2 = NetworkRequestHelper::HttpGet(content2);
                showLog("Content=" + content2);
            }

            QList<QSharedPointer<ProxyEntity>> in; // 更新前
            QList<QSharedPointer<ProxyEntity>> out_all; // 更新前 + 更新后
            QList<QSharedPointer<ProxyEntity>> out; // 更新后
            QList<QSharedPointer<ProxyEntity>> only_in; // 只在更新前有的
            QList<QSharedPointer<ProxyEntity>> only_out; // 只在更新后有的
            QList<QSharedPointer<ProxyEntity>> update_del; // 更新前后都有的，删除更新后多余的

            if (group != nullptr) {// 订阅更新前
                in = group->Profiles();
            }

            // 解析并添加 profile
            update(content2);

            if (group != nullptr) {// 订阅更新完
                out_all = group->Profiles();

                ProfileFilter::OnlyInSrc_ByPointer(out_all, in, out);
                ProfileFilter::OnlyInSrc(in, out, only_in);
                ProfileFilter::OnlyInSrc(out, in, only_out);
                ProfileFilter::Common(in, out, update_del, false, true);
                update_del += only_in;

                for (const auto &ent: update_del) {
                    dataStore->updated_count--;
                    profileManager->DeleteProfile(ent->id);
                }

                qDebug() << "in" << in;
                qDebug() << "out_all" << out_all;
                qDebug() << "out" << out;
                qDebug() << "only_in" << only_in;
                qDebug() << "only_out" << only_out;
                qDebug() << "update_del" << update_del;

                QString notice_added;
                for (const auto &ent: only_out) {
                    notice_added += ent->bean->DisplayTypeAndName() + "\n";
                }
                QString notice_deleted;
                for (const auto &ent: only_in) {
                    notice_deleted += ent->bean->DisplayTypeAndName() + "\n";
                }
                runOnUiThread([=] {
                    MessageBoxWarning("变化", QString("增加了 %1 个代理：\n%2\n减少了 %3 个代理：\n%4").
                            arg(only_out.length()).arg(notice_added).
                            arg(only_in.length()).arg(notice_deleted));
                    GetMainWindow()->dialog_message("SubUpdater", "finish-dingyue");
                });
            } else {
                runOnUiThread([=] {
                    GetMainWindow()->dialog_message("SubUpdater", "finish");
                });
            }
        });
    }
}
