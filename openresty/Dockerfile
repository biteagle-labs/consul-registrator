FROM openresty/openresty:alpine

RUN apk add --no-cache curl perl
RUN opm get ledgetech/lua-resty-http

EXPOSE 80

CMD ["openresty", "-g", "daemon off;"]
