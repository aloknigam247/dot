local backend = {}
local channel
local client_id, max_encoded_bytes = ...

if #vim.api.nvim_get_runtime_file("lua/vim/ui/img.lua", false) == 0 then
  return false
end

local original = require("vim.ui.img")
vim.validate("vim.ui.img.set", original.set, "function")
vim.validate("vim.ui.img.get", original.get, "function")
vim.validate("vim.ui.img.del", original.del, "function")

for _, info in ipairs(vim.api.nvim_list_chans()) do
  local attributes = info.client and info.client.attributes
  if attributes and attributes.dot_image_backend == client_id then
    assert(not channel, "Ambiguous dot image RPC channel")
    channel = info.id
  end
end
assert(channel, "The dot image RPC channel was not found")

local function request(action, ...)
  vim.cmd("redraw")
  return vim.rpcrequest(channel, "dot_image", action, ...)
end

function backend._supported(opts)
  if opts ~= nil then
    vim.validate("opts", opts, "table")
    if opts.chan ~= nil and opts.chan ~= channel then
      return false, "The dot image backend belongs to another UI channel"
    end
  end
  local info = vim.api.nvim_get_chan_info(channel)
  local attributes = info.client and info.client.attributes
  return info.mode == "rpc" and attributes ~= nil and attributes.dot_image_backend == client_id,
    "dot native image backend"
end

function backend.del(id)
  if id == math.huge then
    return request("delete_all")
  end
  return request("delete", id)
end

function backend.get(id)
  local result = request("get", id)
  if result == vim.NIL then
    return nil
  end
  return result
end

function backend.set(data_or_id, opts)
  if type(data_or_id) == "string" and #data_or_id > max_encoded_bytes then
    error(("PNG exceeds the encoded image limit (%d bytes)"):format(max_encoded_bytes), 2)
  end
  return request("set", data_or_id, opts == nil and vim.empty_dict() or opts)
end

vim.ui.img = backend
package.loaded["vim.ui.img"] = backend
return true
