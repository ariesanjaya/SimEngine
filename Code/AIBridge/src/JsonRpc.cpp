#include "JsonRpc.h"

#include <optional>

namespace sim::ai {
namespace {

using nlohmann::json;

const char* DefaultMessage(RpcError code) {
    switch (code) {
        case RpcError::Parse:
            return "Parse error";
        case RpcError::InvalidRequest:
            return "Invalid Request";
        case RpcError::MethodNotFound:
            return "Method not found";
        case RpcError::InvalidParams:
            return "Invalid params";
        case RpcError::Internal:
            return "Internal error";
    }
    return "Internal error";
}

json ErrorObject(RpcError code, std::string_view message, const json& data) {
    json error{{"code", static_cast<int>(code)},
               {"message", message.empty() ? DefaultMessage(code) : std::string(message)}};
    if (!data.is_null()) {
        error["data"] = data;
    }
    return error;
}

json ErrorResponse(const json& id, RpcError code, std::string_view message,
                   const json& data = json()) {
    // `id` null saat permintaannya rusak sampai identitasnya pun tidak terbaca.
    // Spesifikasi menuntut kuncinya tetap ada — klien mencocokkan balasan lewat
    // `id`, dan balasan tanpa kunci itu tidak bisa dipasangkan dengan apa pun.
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", ErrorObject(code, message, data)}};
}

json ResultResponse(const json& id, json result) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

/// Identitas yang sah menurut spesifikasi: string, angka, atau tidak ada.
bool IsValidId(const json& id) { return id.is_string() || id.is_number(); }

/// Menjalankan satu elemen. `nullopt` berarti notifikasi — tidak ada balasan.
std::optional<json> HandleOne(const json& value, const RpcMethod& method) {
    if (!value.is_object()) {
        return ErrorResponse(json(), RpcError::InvalidRequest,
                             "a JSON-RPC request must be an object");
    }

    const json id = value.contains("id") ? value.at("id") : json();
    const bool isNotification = !value.contains("id");
    if (!isNotification && !IsValidId(id)) {
        return ErrorResponse(json(), RpcError::InvalidRequest,
                             "\"id\" must be a string or a number");
    }

    // Versi diperiksa sebelum apa pun yang lain. Klien yang mengirim 1.0
    // memakai bentuk balasan yang berbeda, dan menjawabnya dengan bentuk 2.0
    // membuat kegagalannya muncul di tempat yang jauh dari sebabnya.
    if (!value.contains("jsonrpc") || !value.at("jsonrpc").is_string() ||
        value.at("jsonrpc").get<std::string>() != "2.0") {
        if (isNotification) {
            return std::nullopt;
        }
        return ErrorResponse(id, RpcError::InvalidRequest, "\"jsonrpc\" must be \"2.0\"");
    }

    if (!value.contains("method") || !value.at("method").is_string()) {
        if (isNotification) {
            return std::nullopt;
        }
        return ErrorResponse(id, RpcError::InvalidRequest, "\"method\" must be a string");
    }

    RpcRequest request;
    request.method = value.at("method").get<std::string>();
    request.params = value.contains("params") ? value.at("params") : json();
    request.id = id;
    request.isNotification = isNotification;

    // Params yang ada tapi bukan objek/array adalah permintaan yang salah
    // bentuk, bukan params yang salah isi — karena itu InvalidRequest.
    if (!request.params.is_null() && !request.params.is_object() &&
        !request.params.is_array()) {
        if (isNotification) {
            return std::nullopt;
        }
        return ErrorResponse(id, RpcError::InvalidRequest,
                             "\"params\" must be an object or an array");
    }

    try {
        json result = method(request);
        if (isNotification) {
            return std::nullopt;
        }
        return ResultResponse(id, std::move(result));
    } catch (const RpcException& error) {
        if (isNotification) {
            return std::nullopt;
        }
        return ErrorResponse(id, error.Code(), error.what(), error.Data());
    } catch (const std::exception& error) {
        // **Lemparan yang tidak dikenal tidak boleh keluar dari sini.** Yang di
        // ujung sana adalah soket; pengecualian yang lolos mematikan thread
        // jaringan dan agen melihatnya sebagai koneksi yang putus tanpa sebab.
        if (isNotification) {
            return std::nullopt;
        }
        return ErrorResponse(id, RpcError::Internal, error.what());
    }
}

}  // namespace

std::string MakeRpcErrorResponse(RpcError code, std::string_view message) {
    return ErrorResponse(json(), code, message).dump();
}

std::string DispatchJsonRpc(std::string_view payload, const RpcMethod& method) {
    const json parsed = json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) {
        return MakeRpcErrorResponse(RpcError::Parse, "");
    }

    if (parsed.is_array()) {
        if (parsed.empty()) {
            return ErrorResponse(json(), RpcError::InvalidRequest, "an empty batch is not a request")
                .dump();
        }
        json responses = json::array();
        for (const json& element : parsed) {
            if (std::optional<json> response = HandleOne(element, method)) {
                responses.push_back(std::move(*response));
            }
        }
        // Batch yang seluruhnya notifikasi tidak menghasilkan balasan apa pun,
        // dan array kosong bukan jawaban yang sah untuk itu.
        return responses.empty() ? std::string() : responses.dump();
    }

    if (std::optional<json> response = HandleOne(parsed, method)) {
        return response->dump();
    }
    return {};
}

}  // namespace sim::ai
