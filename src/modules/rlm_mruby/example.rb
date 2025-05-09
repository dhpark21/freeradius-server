#This is example radius.rb script

# frozen_string_literal: true

module Radiusd
    def self.instantiate
        radlog(L_DBG, "[mruby]Running ruby instantiate")
        return RLM_MODULE_OK
    end
    def self.authenticate(request)
        radlog(L_DBG, "[mruby]Running ruby authenticate")
        return RLM_MODULE_NOOP
    end
    def self.recv_access_request(request)
        radlog(L_ERR, "[mruby]Running ruby recv_access_request")
        radlog(L_WARN, "Authorize: #{request.inspect}(#{request.class})")
        radlog(L_WARN, "Authorize: #{request.request.inspect}(#{request.request.class})")
        reply = [["Framed-MTU", 1500]]
        control = [["Password.Cleartext", "hello"], ["Tmp-String-0", "!*", "ANY"]]
        return [RLM_MODULE_UPDATED, reply, control]
    end
    def self.send_access_accept(request)
        radlog(L_DBG, "[mruby]Running ruby send_access_accept")
        return RLM_MODULE_NOOP
    end
    def self.recv_accounting_request(request)
        radlog(L_DBG, "[mruby]Running ruby accounting")
        return RLM_MODULE_NOOP
    end
end
