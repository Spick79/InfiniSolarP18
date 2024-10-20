#ifndef Attribute_Request_h
#define Attribute_Request_h

// Local includes.
#include "Attribute_Request_Callback.h"
#include "IAPI_Implementation.h"


// Attribute request API topics.
char constexpr ATTRIBUTE_REQUEST_TOPIC[] = "v1/devices/me/attributes/request/%u";
char constexpr ATTRIBUTE_RESPONSE_SUBSCRIBE_TOPIC[] = "v1/devices/me/attributes/response/+";
char constexpr ATTRIBUTE_RESPONSE_TOPIC[] = "v1/devices/me/attributes/response/";
// Client side attribute request keys.
char constexpr CLIENT_REQUEST_KEYS[] = "clientKeys";
char constexpr CLIENT_RESPONSE_KEY[] = "client";
// Shared attribute request keys.
char constexpr SHARED_REQUEST_KEY[] = "sharedKeys";
// Log messages.
#if THINGSBOARD_ENABLE_DEBUG
char constexpr NO_KEYS_TO_REQUEST[] = "No keys to request were given";
char constexpr ATT_KEY_NOT_FOUND[] = "Attribute key in Attribute_Request_Callback is NULL";
char constexpr ATT_KEY_IS_NULL[] = "Requested attribute key is NULL";
#endif // THINGSBOARD_ENABLE_DEBUG
#if !THINGSBOARD_ENABLE_DYNAMIC
char constexpr CLIENT_SHARED_ATTRIBUTE_SUBSCRIPTIONS[] = "client or shared attribute request";
#endif // THINGSBOARD_ENABLE_DYNAMIC


/// @brief Handles the internal implementation of the ThingsBoard shared and server-side Attribute API.
/// More specifically it handles the part for both types, where we can request the current value from the cloud
/// See https://thingsboard.io/docs/reference/mqtt-api/#request-attribute-values-from-the-server for more information
/// @tparam Logger Implementation that should be used to print error messages generated by internal processes and additional debugging messages if THINGSBOARD_ENABLE_DEBUG is set, default = DefaultLogger
#if THINGSBOARD_ENABLE_DYNAMIC
template <typename Logger = DefaultLogger>
#else
/// @tparam MaxSubscriptions Maximum amount of simultaneous server side rpc subscriptions.
/// Once the maximum amount has been reached it is not possible to increase the size, this is done because it allows to allcoate the memory on the stack instead of the heap, default = Default_Subscriptions_Amount (1)
/// @tparam MaxAttributes Maximum amount of attributes that will ever be requested with the Attribute_Request_Callback, allows to use an array on the stack in the background, default = Default_Attributes_Amount (5)
template<size_t MaxSubscriptions = Default_Subscriptions_Amount, size_t MaxAttributes = Default_Attributes_Amount, typename Logger = DefaultLogger>
#endif // THINGSBOARD_ENABLE_DYNAMIC
class Attribute_Request : public IAPI_Implementation {
  public:
    /// @brief Constructor
    Attribute_Request() = default;

    /// @brief Requests one client-side attribute calllback,
    /// that will be called if the key-value pair from the server for the given client-side attributes is received.
    /// Because the client-side attribute request is a single event subscription, meaning we only ever receive a response to our request once,
    /// we automatically unsubscribe and delete the internal allocated data for the request as soon as the response has been received and handled by the subscribed callback.
    /// See https://thingsboard.io/docs/reference/mqtt-api/#request-attribute-values-from-the-server for more information
    /// @param callback Callback method that will be called
    /// @return Whether requesting the given callback was successful or not
#if THINGSBOARD_ENABLE_DYNAMIC
    bool Client_Attributes_Request(Attribute_Request_Callback const & callback) {
#else
    bool Client_Attributes_Request(Attribute_Request_Callback<MaxAttributes> const & callback) {
#endif // THINGSBOARD_ENABLE_DYNAMIC
        return Attributes_Request(callback, CLIENT_REQUEST_KEYS, CLIENT_RESPONSE_KEY);
    }

    /// @brief Requests one shared attribute calllback,
    /// that will be called if the key-value pair from the server for the given shared attributes is received.
    /// Because the shared attribute request is a single event subscription, meaning we only ever receive a response to our request once,
    /// we automatically unsubscribe and delete the internal allocated data for the request as soon as the response has been received and handled by the subscribed callback.
    /// See https://thingsboard.io/docs/reference/mqtt-api/#request-attribute-values-from-the-server for more information
    /// @param callback Callback method that will be called
    /// @return Whether requesting the given callback was successful or not
#if THINGSBOARD_ENABLE_DYNAMIC
    bool Shared_Attributes_Request(Attribute_Request_Callback const & callback) {
#else
    bool Shared_Attributes_Request(Attribute_Request_Callback<MaxAttributes> const & callback) {
#endif // THINGSBOARD_ENABLE_DYNAMIC
        return Attributes_Request(callback, SHARED_REQUEST_KEY, SHARED_RESPONSE_KEY);
    }

    API_Process_Type Get_Process_Type() const override {
        return API_Process_Type::JSON;
    }

    void Process_Response(char const * topic, uint8_t * payload, unsigned int length) override {
        // Nothing to do
    }

    void Process_Json_Response(char const * topic, JsonDocument const & data) override {
        size_t const request_id = Helper::parseRequestId(ATTRIBUTE_RESPONSE_TOPIC, topic);
        JsonObjectConst object = data.template as<JsonObjectConst>();

#if THINGSBOARD_ENABLE_STL
#if THINGSBOARD_ENABLE_DYNAMIC
        auto it = std::find_if(m_attribute_request_callbacks.begin(), m_attribute_request_callbacks.end(), [&request_id](Attribute_Request_Callback & attribute_request) {
#else
        auto it = std::find_if(m_attribute_request_callbacks.begin(), m_attribute_request_callbacks.end(), [&request_id](Attribute_Request_Callback<MaxAttributes> & attribute_request) {
#endif // THINGSBOARD_ENABLE_DYNAMIC
            return attribute_request.Get_Request_ID() == request_id;
        });
        if (it != m_attribute_request_callbacks.end()) {
            auto & attribute_request = *it;
#else
        for (auto it = m_attribute_request_callbacks.begin(); it != m_attribute_request_callbacks.end(); ++it) {
            auto & attribute_request = *it;

            if (attribute_request.Get_Request_ID() != request_id) {
                continue;
            }
#endif // THINGSBOARD_ENABLE_STL
            char const * attribute_response_key = attribute_request.Get_Attribute_Key();
            if (attribute_response_key == nullptr) {
#if THINGSBOARD_ENABLE_DEBUG
                Logger::println(ATT_KEY_NOT_FOUND);
#endif // THINGSBOARD_ENABLE_DEBUG
                goto delete_callback;
            }

            if (object.containsKey(attribute_response_key)) {
                object = object[attribute_response_key];
            }

            attribute_request.Stop_Timeout_Timer();
            attribute_request.Call_Callback(object);

            delete_callback:
            // Delete callback because the changes have been requested and the callback is no longer needed
            Helper::remove(m_attribute_request_callbacks, it);
#if !THINGSBOARD_ENABLE_STL
            break;
#endif // !THINGSBOARD_ENABLE_STL
        }

        // Unsubscribe from the shared attribute request topic,
        // if we are not waiting for any further responses with shared attributes from the server.
        // Will be resubscribed if another request is sent anyway
        if (m_attribute_request_callbacks.empty()) {
            (void)Attributes_Request_Unsubscribe();
        }
    }

    bool Compare_Response_Topic(char const * topic) const override {
        return strncmp(ATTRIBUTE_RESPONSE_TOPIC, topic, strlen(ATTRIBUTE_RESPONSE_TOPIC)) == 0;
    }

    bool Unsubscribe() override {
        return Attributes_Request_Unsubscribe();
    }

    bool Resubscribe_Topic() override {
        return Unsubscribe();
    }

#if !THINGSBOARD_USE_ESP_TIMER
    void loop() override {
        for (auto & attribute_request : m_attribute_request_callbacks) {
            attribute_request.Update_Timeout_Timer();
        }
    }
#endif // !THINGSBOARD_USE_ESP_TIMER

    void Initialize() override {
        // Nothing to do
    }

    void Set_Client_Callbacks(Callback<void, IAPI_Implementation &>::function subscribe_api_callback, Callback<bool, char const * const, JsonDocument const &, size_t const &>::function send_json_callback, Callback<bool, char const * const, char const * const>::function send_json_string_callback, Callback<bool, char const * const>::function subscribe_topic_callback, Callback<bool, char const * const>::function unsubscribe_topic_callback, Callback<uint16_t>::function get_size_callback, Callback<bool, uint16_t>::function set_buffer_size_callback, Callback<size_t *>::function get_request_id_callback) override {
        m_send_json_callback.Set_Callback(send_json_callback);
        m_subscribe_topic_callback.Set_Callback(subscribe_topic_callback);
        m_unsubscribe_topic_callback.Set_Callback(unsubscribe_topic_callback);
        m_get_request_id_callback.Set_Callback(get_request_id_callback);
    }

  private:
    /// @brief Requests one client-side or shared attribute calllback,
    /// that will be called if the key-value pair from the server for the given client-side or shared attributes is received
    /// @param callback Callback method that will be called
    /// @param attribute_request_key Key of the key-value pair that will contain the attributes we want to request
    /// @param attribute_response_key Key of the key-value pair that will contain the attributes we got as a response
    /// @return Whether requesting the given callback was successful or not
#if THINGSBOARD_ENABLE_DYNAMIC
    bool Attributes_Request(Attribute_Request_Callback const & callback, char const * attribute_request_key, char const * attribute_response_key) {
#else
    bool Attributes_Request(Attribute_Request_Callback<MaxAttributes> const & callback, char const * attribute_request_key, char const * attribute_response_key) {
#endif // THINGSBOARD_ENABLE_DYNAMIC
        auto const & attributes = callback.Get_Attributes();

        // Check if any sharedKeys were requested
        if (attributes.empty()) {
#if THINGSBOARD_ENABLE_DEBUG
            Logger::println(NO_KEYS_TO_REQUEST);
#endif // THINGSBOARD_ENABLE_DEBUG
            return false;
        }
        else if (attribute_request_key == nullptr || attribute_response_key == nullptr) {
#if THINGSBOARD_ENABLE_DEBUG
            Logger::println(ATT_KEY_NOT_FOUND);
#endif // THINGSBOARD_ENABLE_DEBUG
            return false;
        }

#if THINGSBOARD_ENABLE_DYNAMIC
        Attribute_Request_Callback * registered_callback = nullptr;
#else
        Attribute_Request_Callback<MaxAttributes> * registered_callback = nullptr;
#endif // THINGSBOARD_ENABLE_DYNAMIC
        if (!Attributes_Request_Subscribe(callback, registered_callback)) {
            return false;
        }
        else if (registered_callback == nullptr) {
            return false;
        }

        // String are const char* and therefore stored as a pointer --> zero copy, meaning the size for the strings is 0 bytes,
        // Data structure size depends on the amount of key value pairs passed + the default clientKeys or sharedKeys
        // See https://arduinojson.org/v6/assistant/ for more information on the needed size for the JsonDocument
        StaticJsonDocument<JSON_OBJECT_SIZE(1)> request_buffer;

        // Calculate the size required for the char buffer containing all the attributes seperated by a comma,
        // before initalizing it so it is possible to allocate it on the stack
        size_t size = 0U;
        for (const auto & att : attributes) {
            if (Helper::stringIsNullorEmpty(att)) {
                continue;
            }

            size += strlen(att);
            size += strlen(",");
        }

        // Initalizes complete array to 0, required because strncat needs both destination and source to contain proper null terminated strings
        char request[size] = {};
        for (const auto & att : attributes) {
            if (Helper::stringIsNullorEmpty(att)) {
#if THINGSBOARD_ENABLE_DEBUG
                Logger::println(ATT_KEY_IS_NULL);
#endif // THINGSBOARD_ENABLE_DEBUG
                continue;
            }

            strncat(request, att, size);
            size -= strlen(att);
            strncat(request, ",", size);
            size -= strlen(",");
        }

        // Ensure to cast to const, this is done so that ArduinoJson does not copy the value but instead simply store the pointer, which does not require any more memory,
        // besides the base size needed to allocate one key-value pair. Because if we don't the char array would be copied
        // and because there is not enough space the value would simply be "undefined" instead. Which would cause the request to not be sent correctly
        request_buffer[attribute_request_key] = static_cast<const char*>(request);

        size_t * p_request_id = m_get_request_id_callback.Call_Callback();
        if (p_request_id == nullptr) {
            Logger::println(REQUEST_ID_NULL);
            return false;
        }
        auto & request_id = *p_request_id;

        registered_callback->Set_Request_ID(++request_id);
        registered_callback->Set_Attribute_Key(attribute_response_key);
        registered_callback->Start_Timeout_Timer();

        char topic[Helper::detectSize(ATTRIBUTE_REQUEST_TOPIC, request_id)] = {};
        (void)snprintf(topic, sizeof(topic), ATTRIBUTE_REQUEST_TOPIC, request_id);
        return m_send_json_callback.Call_Callback(topic, request_buffer, Helper::Measure_Json(request_buffer));
    }

    /// @brief Subscribes to attribute response topic
    /// @param callback Callback method that will be called
    /// @param registered_callback Editable pointer to a reference of the local version that was copied from the passed callback
    /// @return Whether requesting the given callback was successful or not
#if THINGSBOARD_ENABLE_DYNAMIC
    bool Attributes_Request_Subscribe(Attribute_Request_Callback const & callback, Attribute_Request_Callback * & registered_callback) {
#else
    bool Attributes_Request_Subscribe(Attribute_Request_Callback<MaxAttributes> const & callback, Attribute_Request_Callback<MaxAttributes> * & registered_callback) {
#endif // THINGSBOARD_ENABLE_DYNAMIC
#if !THINGSBOARD_ENABLE_DYNAMIC
        if (m_attribute_request_callbacks.size() + 1 > m_attribute_request_callbacks.capacity()) {
            Logger::printfln(MAX_SUBSCRIPTIONS_EXCEEDED, MAX_SUBSCRIPTIONS_TEMPLATE_NAME, CLIENT_SHARED_ATTRIBUTE_SUBSCRIPTIONS);
            return false;
        }
#endif // !THINGSBOARD_ENABLE_DYNAMIC
        if (!m_subscribe_topic_callback.Call_Callback(ATTRIBUTE_RESPONSE_SUBSCRIBE_TOPIC)) {
            Logger::printfln(SUBSCRIBE_TOPIC_FAILED, ATTRIBUTE_RESPONSE_SUBSCRIBE_TOPIC);
          return false;
        }
        m_attribute_request_callbacks.push_back(callback);
        registered_callback = &m_attribute_request_callbacks.back();
        return true;
    }

    /// @brief Unsubscribes all client-side or shared attributes request callbacks
    /// @return Whether unsubcribing the previously subscribed callbacks
    /// and from the  attribute response topic, was successful or not
    bool Attributes_Request_Unsubscribe() {
        m_attribute_request_callbacks.clear();
        return m_unsubscribe_topic_callback.Call_Callback(ATTRIBUTE_RESPONSE_SUBSCRIBE_TOPIC);
    }

    Callback<bool, char const * const, JsonDocument const &, size_t const &> m_send_json_callback = {};          // Send json document callback
    Callback<bool, char const * const>                                       m_subscribe_topic_callback = {};    // Subscribe mqtt topic client callback
    Callback<bool, char const * const>                                       m_unsubscribe_topic_callback = {};  // Unubscribe mqtt topic client callback
    Callback<size_t *>                                                       m_get_request_id_callback = {};     // Get internal request id callback

    // Vectors or array (depends on wheter if THINGSBOARD_ENABLE_DYNAMIC is set to 1 or 0), hold copy of the actual passed data, this is to ensure they stay valid,
    // even if the user only temporarily created the object before the method was called.
    // This can be done because all Callback methods mostly consists of pointers to actual object so copying them
    // does not require a huge memory overhead and is acceptable especially in comparsion to possible problems that could
    // arise if references were used and the end user does not take care to ensure the Callbacks live on for the entirety
    // of its usage, which will lead to dangling references and undefined behaviour.
    // Therefore copy-by-value has been choosen as for this specific use case it is more advantageous,
    // especially because at most we copy internal vectors or array, that will only ever contain a few pointers
#if THINGSBOARD_ENABLE_DYNAMIC
    Vector<Attribute_Request_Callback>                                       m_attribute_request_callbacks = {}; // Client-side or shared attribute request callback vector
#else
    Array<Attribute_Request_Callback<MaxAttributes>, MaxSubscriptions>       m_attribute_request_callbacks = {}; // Client-side or shared attribute request callback array
#endif // THINGSBOARD_ENABLE_DYNAMIC
};

#endif // Attribute_Request_h
