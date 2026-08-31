-- The smallest possible library, to prove the mechanism before anything
-- depends on it.
return {
  greet = function(who)
    return "hello, " .. tostring(who) .. ", from /lib"
  end,
}
